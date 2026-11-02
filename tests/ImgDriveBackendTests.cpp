#include "flaccd/mount/ImgDriveBackend.h"
#include "flaccd/CueSheetWriter.h"
#include "flaccd/PathText.h"

#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <random>
#include <stdexcept>
#include <thread>

namespace {

void require(bool condition, std::string_view message) {
   if (!condition) {
      throw std::runtime_error(std::string(message));
   }
}

template<typename Operation>
void expectError(Operation operation, std::string_view expected = {}) {
   try {
      operation();
   } catch (const std::exception& error) {
      require(std::string_view(error.what()).find(expected) != std::string_view::npos,
              std::string("unexpected diagnostic: ") + error.what());
      return;
   }
   throw std::runtime_error("expected operation to fail");
}

std::string readFile(const std::filesystem::path& path) {
   std::ifstream input(path, std::ios::binary);
   require(static_cast<bool>(input), "open test file");
   return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void writeFile(const std::filesystem::path& path, const std::string& contents) {
   std::ofstream output(path, std::ios::binary);
   output << contents;
   output.close();
   require(static_cast<bool>(output), "write test file");
}

class TestDirectory {
public:
   explicit TestDirectory(const std::filesystem::path& parent) {
      std::random_device generator;
      for (int attempt = 0; attempt < 100; ++attempt) {
         path = parent / ("imgdrive-test-" + std::to_string(generator()));
         if (std::filesystem::create_directory(path)) {
            return;
         }
      }
      throw std::runtime_error("cannot create test directory");
   }
   ~TestDirectory() {
      std::error_code error;
      std::filesystem::remove_all(path, error);
   }
   std::filesystem::path path;
};

class FakeHost : public flaccd::ProcessRunner {
public:
   flaccd::ProcessResult run(const std::filesystem::path& executable,
                             std::span<const std::string> arguments,
                             std::chrono::milliseconds timeout) override {
      require(std::filesystem::is_regular_file(executable) && timeout.count() > 0, "process configuration");
      calls.emplace_back(arguments.begin(), arguments.end());
      if (arguments[0] == "-l") {
         return {0, false, "Vendor drive listing (unparsed)\n"};
      }
      require(arguments.size() == 2, "mounter must receive distinct arguments, never a shell command");
      const auto cuePath = flaccd::pathFromUtf8(arguments[1]);
      require(std::filesystem::exists(cuePath), "CUE must survive through process invocation");
      flaccd::MountStateStore state(cuePath.parent_path().parent_path());
      const auto records = state.records();
      require(records.size() == 1, "state must be published before invocation");
      require(records[0].state == (arguments[0] == "-u" ? flaccd::MountState::Unmounting : flaccd::MountState::Mounting),
              "phase must persist before process invocation");
      if (arguments[0] != "-u") {
         const auto persisted = flaccd::parseCueTextMetadata(readFile(cuePath));
         const auto reference = flaccd::pathFromUtf8(persisted.fileName);
         require(persisted.errors.empty() && persisted.fileType == "WAVE" && reference.is_absolute(),
                 "persisted mount CUE must contain an absolute WAVE reference");
         require(std::filesystem::equivalent(reference, records[0].sourcePath),
                 "persisted mount CUE references a different audio file");
         std::ifstream original(reference, std::ios::binary);
         char signature[4]{};
         original.read(signature, sizeof(signature));
         require(original && std::string_view(signature, sizeof(signature)) == "fLaC",
                 "mock mounter cannot open original FLAC at process invocation");
         require(!std::filesystem::exists(cuePath.parent_path() / "disc.flac"), "mount created a disc.flac alias");
         for (const auto& entry : std::filesystem::directory_iterator(cuePath.parent_path())) {
            require(!std::filesystem::is_symlink(entry.path()), "normal mount created a cache symlink");
            require(!std::filesystem::equivalent(entry.path(), reference), "normal mount created an audio hard link");
            require(!readFile(entry.path()).starts_with("fLaC"), "normal mount copied FLAC into the cache");
         }
         pendingCue = cuePath;
      }
      if (result.exitCode == 0 && !result.timedOut && changeMedia) {
         media = arguments[0] == "-u" ? flaccd::MediaState::Empty : flaccd::MediaState::Ready;
      }
      return result;
   }
   flaccd::MediaState inspectDrive(char) override {
      return media;
   }
   bool matchesLayout(char, const flaccd::TrackLayout&) override {
      return layoutMatches;
   }
   std::optional<std::vector<char>> emptyDrives() override {
      return available;
   }
   std::unique_ptr<flaccd::MountLock> lock(const std::filesystem::path&, std::chrono::milliseconds) override {
      return std::make_unique<flaccd::MountLock>();
   }
   void pause(std::chrono::milliseconds) override {
      if (readyAfterPauses > 0) {
         require(std::filesystem::exists(pendingCue), "CUE deleted during asynchronous mount completion");
         const auto parsed = flaccd::parseCueTextMetadata(readFile(pendingCue));
         require(std::filesystem::is_regular_file(flaccd::pathFromUtf8(parsed.fileName)),
                 "FLAC reference disappeared after mounter process exit");
         if (--readyAfterPauses == 0) {
            media = flaccd::MediaState::Ready;
         }
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
   }

   flaccd::ProcessResult result;
   flaccd::MediaState media = flaccd::MediaState::Empty;
   bool layoutMatches = true;
   bool changeMedia = true;
   int readyAfterPauses = 0;
   std::filesystem::path pendingCue;
   std::optional<std::vector<char>> available;
   std::vector<std::vector<std::string>> calls;
};

void testCue(const std::filesystem::path& source) {
   const auto metadata = flaccd::FlacMetadataInspector{}.inspect(source);
   for (const auto selection : {"native", "text", "auto"}) {
      const auto original = flaccd::selectTrackLayout(metadata, selection);
      const auto cue = flaccd::generateFlacCueSheet(original, metadata, source);
      const auto parsed = flaccd::parseCueTextMetadata(cue);
      require(parsed.errors.empty() && parsed.fileType == "WAVE", "FLAC-backed FILE must use WAVE");
      require(std::filesystem::equivalent(flaccd::pathFromUtf8(parsed.fileName), source), "wrong FLAC referenced");
      require(flaccd::pathFromUtf8(parsed.fileName).is_absolute(), "cache CUE must use absolute FLAC reference");
#ifdef _WIN32
      // On Windows the generated string itself must be fully qualified, not
      // merely a drive-relative path that filesystem::equivalent can resolve.
      const auto& name = parsed.fileName;
      const bool drivePath = name.size() >= 3 && name[1] == ':' && (name[2] == '\\' || name[2] == '/');
      const bool uncPath = name.starts_with("\\\\") || name.starts_with("//");
      require(drivePath || uncPath, "generated FILE must use an absolute Windows drive/UNC path");
      require(!name.starts_with("\\\\?\\"), "CUE must not contain a Win32 extended-device path prefix");
      require(name.find("Music & spaces; [音楽 é].flac") != std::string::npos,
              "Windows FILE reference lost spaces or Unicode filename bytes");
#endif
      require(cue.find("INDEX 00 00:00:00") != std::string::npos &&
              cue.find("INDEX 01 00:00:02") != std::string::npos, "CUE lost pregap or added two seconds");
      require(parsed.title == "Fixture Album" && parsed.performer == "Fixture Album Artist", "descriptive metadata lost");
      const auto roundTrip = flaccd::TrackLayout::fromText(metadata, parsed);
      require(flaccd::compareTrackLayouts(original, roundTrip).empty(), "FLAC CUE round-trip differs");
      for (const auto& track : original.getTracks()) {
         for (const auto& index : track.indices) {
            const auto left = original.queryLba(index.discLba);
            const auto right = roundTrip.queryLba(index.discLba);
            require(left && right && left->trackNumber == right->trackNumber &&
                    left->indexNumber == right->indexNumber && left->sourceSector == right->sourceSector,
                    "track/index boundary mapping changed");
         }
      }
   }
   auto text = flaccd::parseCueTextMetadata(metadata.embeddedCueSheets.front());
   text.title = "Disc title";
   text.performer = "Disc performer";
   text.tracks[0].title = "Track title";
   text.tracks[0].performer = "Guest performer";
   text.tracks[0].isrc = "USABC2600001";
   const auto described = flaccd::TrackLayout::fromText(metadata, text);
   const auto parsed = flaccd::parseCueTextMetadata(flaccd::generateFlacCueSheet(described, metadata, source));
   require(parsed.title == text.title && parsed.performer == text.performer &&
           parsed.tracks[0].title == text.tracks[0].title && parsed.tracks[0].performer == text.tracks[0].performer &&
           parsed.tracks[0].isrc == text.tracks[0].isrc && parsed.tracks[0].preEmphasis &&
           parsed.tracks[0].digitalCopyPermitted, "FLAC-backed CUE lost track descriptions, ISRC, or PRE/DCP");
}

void testLifecycle(const std::filesystem::path& source,
                   const std::filesystem::path& executable,
                   const std::filesystem::path& cache) {
   FakeHost host;
   std::filesystem::path cuePath;
   {
      flaccd::ImgDriveBackend backend(host, executable, cache);
      expectError([&] { backend.mount(source); }, "enumeration");
      require(host.calls.empty(), "unverified enumeration invoked mounter");
      require(backend.drives().find("Vendor drive listing") != std::string::npos, "raw drives listing");
      const auto record = backend.mount(source, 'd');
      cuePath = record.cuePath;
      require(record.state == flaccd::MountState::Mounted && host.calls.back()[0] == "-m:D", "mount invocation/state");
      require(std::filesystem::exists(cuePath), "mounted CUE was prematurely removed");
      const auto commandCount = host.calls.size();
      expectError([&] { backend.mount(source, 'D'); }, "already has FlacCD state");
      expectError([&] { backend.unmount('E'); }, "no FlacCD-owned");
      require(host.calls.size() == commandCount, "already-mounted/unrelated drive caused a process invocation");
   }
   require(std::filesystem::exists(cuePath), "CUE removed when mounting process exited");
   {
      flaccd::ImgDriveBackend backend(host, executable, cache);
      const auto status = backend.status();
      require(status.size() == 1 && status[0].media == flaccd::MediaState::Ready, "persistent status");
      const auto movedSource = source.parent_path() / "temporarily-moved.flac";
      std::filesystem::rename(source, movedSource);
      require(backend.status().size() == 1, "status must work when source is missing");
      backend.unmount('D');
      std::filesystem::rename(movedSource, source);
      require(host.calls.back() == std::vector<std::string>{"-u", flaccd::pathToUtf8(cuePath)},
              "unmount must target exact owned CUE, not a drive or all drives");
      require(!std::filesystem::exists(cuePath) && backend.status().empty(), "successful unmount did not clean state");
      host.available = std::vector<char>{'E'};
      const auto remounted = backend.mount(source);
      require(remounted.drive == 'E', "verified enumeration did not select available drive");
      backend.unmount('E');
      host.available = std::vector<char>{};
      expectError([&] { backend.mount(source); }, "no available");
   }
}

void testFailures(const std::filesystem::path& source,
                  const std::filesystem::path& executable,
                  const std::filesystem::path& parent) {
   for (int variant = 0; variant < 4; ++variant) {
      const auto cache = parent / ("failure-" + std::to_string(variant));
      FakeHost host;
      flaccd::ImgDriveBackend backend(host, executable, cache, std::chrono::milliseconds(2));
      if (variant == 0) {
         host.result = {7, false, "vendor failure details"};
      } else if (variant == 1) {
         host.result = {259, true, "slow mounter"};
      } else if (variant == 2) {
         host.layoutMatches = false;
      } else {
         host.changeMedia = false;
      }
      expectError([&] { backend.mount(source, 'D'); }, variant < 2 ? "ImgDrive" : "not confirmed");
      const auto status = backend.status();
      require(status.size() == 1 && status[0].record.state == flaccd::MountState::Mounting &&
              std::filesystem::exists(status[0].record.cuePath), "uncertain mount lost persistent CUE/state");
      host.result = {};
      host.layoutMatches = true;
      host.changeMedia = true;
      // Recovery can unmount a pending/failed/stale record, even when empty.
      backend.unmount('D');
      require(backend.status().empty(), "pending mount recovery did not clean owned state");
   }
   FakeHost host;
   const auto cache = parent / "unmount-failure";
   flaccd::ImgDriveBackend backend(host, executable, cache, std::chrono::milliseconds(2));
   const auto record = backend.mount(source, 'D');
   host.result = {5, false, "unmount denied"};
   expectError([&] { backend.unmount('D'); }, "unmount denied");
   require(std::filesystem::exists(record.cuePath), "failed unmount removed needed CUE");
   host.result = {};
   host.changeMedia = false;
   expectError([&] { backend.unmount('D'); }, "not confirmed");
   require(std::filesystem::exists(record.cuePath), "unconfirmed unmount removed needed CUE");
   host.changeMedia = true;
   backend.unmount('D');
   host.media = flaccd::MediaState::Ready;
   expectError([&] { backend.mount(source, 'D'); }, "occupied");
   host.media = flaccd::MediaState::Unknown;
   expectError([&] { backend.mount(source, 'D'); }, "occupied");
   expectError([&] { backend.mount(source, 'C'); }, "D: through Z:");

   FakeHost missing;
   expectError([&] { flaccd::ImgDriveBackend bad(missing, parent / "absent.exe", parent / "missing"); }, "missing");
   writeFile(parent / "invalid.exe", "Not a PE executable");
   expectError([&] { flaccd::ImgDriveBackend bad(missing, parent / "invalid.exe", parent / "invalid"); }, "invalid");
}

void testAsyncAndPreflight(const std::filesystem::path& source,
                           const std::filesystem::path& executable,
                           const std::filesystem::path& cache) {
   FakeHost host;
   host.changeMedia = false;
   host.readyAfterPauses = 2;
   flaccd::ImgDriveBackend backend(host, executable, cache);
   bool preparedObserved = false;
   const auto mounted = backend.mount(source, 'D', "auto", [&](const flaccd::MountRecord& record) {
      require(host.calls.empty(), "prepared diagnostics must precede process invocation");
      require(readFile(record.cuePath) == record.cueText, "prepared CUE must already exist on disk");
      preparedObserved = true;
   });
   require(preparedObserved && mounted.state == flaccd::MountState::Mounted && host.readyAfterPauses == 0,
           "did not wait for asynchronous completion after process exit");
   host.changeMedia = true;
   backend.unmount('D');
   host.calls.clear();
   const auto moved = source.parent_path() / "source-moved-during-preparation.flac";
   expectError([&] {
      backend.mount(source, 'D', "auto", [&](const flaccd::MountRecord&) {
         std::filesystem::rename(source, moved);
      });
   }, "original FLAC");
   require(host.calls.empty(), "disappearing source reached ImgDrive");
   std::filesystem::rename(moved, source);
   const auto pending = backend.status();
   require(pending.size() == 1 && pending[0].record.state == flaccd::MountState::Prepared &&
           std::filesystem::exists(pending[0].record.cuePath), "preflight error lost recoverable state/CUE");
   backend.unmount('D');
}

void testOwnership(const std::filesystem::path& source, const std::filesystem::path& cache) {
   const auto metadata = flaccd::FlacMetadataInspector{}.inspect(source);
   const auto layout = flaccd::selectTrackLayout(metadata, "auto");
   flaccd::MountStateStore store(cache);
   writeFile(cache / "unrelated.cue", "Keep this unrelated file");
   const auto record = store.create('D', source, flaccd::generateFlacCueSheet(layout, metadata, source));
   auto forged = record;
   forged.cuePath = cache / "unrelated.cue";
   expectError([&] { store.removeDetached(forged); });
   forged = record;
   forged.id = "../unrelated";
   expectError([&] { store.removeDetached(forged); });
   expectError([&] { store.removeDetached(record); }, "confirmed detachment");
   store.mark(record, flaccd::MountState::Detached);
   writeFile(record.cuePath.parent_path() / "foreign.txt", "Keep foreign contents");
   expectError([&] { store.removeDetached(record); }, "unrecognized");
   require(std::filesystem::exists(record.cuePath), "unknown contents caused partial deletion");
   std::filesystem::remove(record.cuePath.parent_path() / "foreign.txt");
   writeFile(record.cuePath, "Somebody else's CUE");
   expectError([&] { store.removeDetached(record); }, "changed");
   writeFile(record.cuePath, record.cueText);
   std::filesystem::remove(record.cuePath);
   std::error_code error;
   std::filesystem::create_symlink(cache / "unrelated.cue", record.cuePath, error);
   if (!error) {
      expectError([&] { store.removeDetached(record); }, "unsafe");
      require(readFile(cache / "unrelated.cue") == "Keep this unrelated file", "symlink target deleted");
      std::filesystem::remove(record.cuePath);
   }
   writeFile(record.cuePath, record.cueText);
   store.removeDetached(record);
   require(readFile(cache / "unrelated.cue") == "Keep this unrelated file", "unrelated cache file deleted");
   const auto interrupted = store.create('E', source, flaccd::generateFlacCueSheet(layout, metadata, source));
   store.mark(interrupted, flaccd::MountState::Mounting);
   flaccd::MountStateStore recovered(cache);
   require(recovered.records().size() == 1 && recovered.records()[0].state == flaccd::MountState::Mounting,
           "interrupted mount state was not recoverable");
   writeFile(interrupted.cuePath.parent_path() / "mount.state", "corrupt manifest");
   expectError([&] { recovered.records(); }, "malformed");
   require(std::filesystem::exists(interrupted.cuePath), "corrupt state triggered unsafe cleanup");
   const auto unowned = cache.parent_path() / "unowned";
   std::filesystem::create_directory(unowned);
   writeFile(unowned / "keep.txt", "unrelated");
   expectError([&] { flaccd::MountStateStore rejected(unowned); }, "ownership");
}

void testQuoting() {
   require(flaccd::quoteWindowsArgument(L"C:\\Music with spaces\\音楽.flac") == L"\"C:\\Music with spaces\\音楽.flac\"",
           "Unicode/spaces command quoting");
   require(flaccd::quoteWindowsArgument(L"a\"b") == L"\"a\\\"b\"", "embedded quote escaping");
   require(flaccd::quoteWindowsArgument(L"end\\") == L"\"end\\\\\"", "trailing backslash escaping");
   require(flaccd::quoteWindowsArgument(L"&;|<>^") == L"\"&;|<>^\"", "shell characters must remain literal arguments");
   expectError([&] { flaccd::quoteWindowsArgument(std::wstring(L"bad\0argument", 12)); }, "NUL");
}

}

int main(int argumentCount, char* arguments[]) {
   try {
      require(argumentCount == 2, "expected fixture directory");
      const auto fixtures = std::filesystem::absolute(arguments[1]);
      TestDirectory directory(fixtures);
      const auto source = directory.path / flaccd::pathFromUtf8("Music & spaces; [音楽 é].flac");
      std::filesystem::copy_file(fixtures / "fixture-4096.flac", source);
      const auto originalBytes = readFile(source);
      const auto executable = directory.path / "ImgDrive mock.exe";
      writeFile(executable, "MZmock");
      testQuoting();
      testCue(source);
      testLifecycle(source, executable, directory.path / flaccd::pathFromUtf8("cache 音楽 & spaces"));
      testFailures(source, executable, directory.path);
      testOwnership(source, directory.path / "ownership");
      testAsyncAndPreflight(source, executable, directory.path / "asynchronous");
      FakeHost host;
      flaccd::ImgDriveBackend backend(host, executable, directory.path / "unsupported");
      expectError([&] { backend.mount(fixtures / "fixture-gap.flac", 'D', "text"); }, "source-less");
      expectError([&] { backend.mount(fixtures / "fixture-no-first-pregap.flac", 'D'); }, "conflict");
      expectError([&] { backend.mount(fixtures / "unsupported-0.flac", 'D'); }, "no valid");
#ifndef _WIN32
      const auto unsafe = directory.path / "unsafe\"name.flac";
      std::filesystem::copy_file(source, unsafe);
      expectError([&] { backend.mount(unsafe, 'D'); }, "quote");
#endif
      require(host.calls.empty() && backend.status().empty(), "invalid layout invoked ImgDrive or created a mount");
      require(readFile(source) == originalBytes, "mounting changed source FLAC");
      for (const auto& entry : std::filesystem::recursive_directory_iterator(directory.path)) {
         require(entry.path().extension() != ".bin", "normal mount path created an uncompressed BIN");
      }
      std::cout << "ImgDrive coordinator/CUE/state tests passed with mocked process and device APIs\n";
   } catch (const std::exception& error) {
      std::cerr << error.what() << '\n';
      return 1;
   }
   return 0;
}
