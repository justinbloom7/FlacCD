#include "flaccd/mount/ImgDriveBackend.h"
#include "flaccd/mount/MountAgent.h"
#include "flaccd/mount/AgentCommand.h"
#include "flaccd/CueSheetWriter.h"
#include "flaccd/PathText.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <future>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <map>
#include <mutex>
#include <random>
#include <sstream>
#include <set>
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

void writeFile(const std::filesystem::path& path, const std::string& text) {
   std::ofstream output(path, std::ios::binary);
   output << text;
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

class TestFileSystem : public flaccd::MountFileSystem {
public:
   std::error_code createCueExclusive(const std::filesystem::path& path, const std::string& text) override {
      if (path == adjacent) {
         if (race) {
            race = false;
            const auto created = MountFileSystem::createCueExclusive(path, racedCue);
            require(!created, "inject racing external CUE");
            return std::make_error_code(std::errc::file_exists);
         }
         if (denyAdjacent) {
            return std::make_error_code(std::errc::read_only_file_system);
         }
      }
      return MountFileSystem::createCueExclusive(path, text);
   }
   std::error_code hardLink(const std::filesystem::path& source, const std::filesystem::path& destination) override {
      ++linkCalls;
      return linkError ? linkError : MountFileSystem::hardLink(source, destination);
   }
   void copyCompressed(const std::filesystem::path& source,
                       const std::filesystem::path& destination,
                       const flaccd::MountLog& log) override {
      ++copyCalls;
      if (failCopy) {
         require(!MountFileSystem::createCueExclusive(destination, "fLaCpartial"), "inject partial copy");
         throw std::runtime_error("simulated compressed copy failure");
      }
      MountFileSystem::copyCompressed(source, destination, log);
   }
   std::filesystem::path adjacent;
   bool denyAdjacent = false;
   bool race = false;
   bool failCopy = false;
   std::string racedCue;
   std::error_code linkError;
   int linkCalls = 0;
   int copyCalls = 0;
};

class TestLock : public flaccd::MountLock {
public:
   explicit TestLock(std::recursive_mutex& mutex) : guard(mutex) {
   }
private:
   std::unique_lock<std::recursive_mutex> guard;
};

class FakeHost : public flaccd::ProcessRunner {
public:
   flaccd::ProcessResult run(const std::filesystem::path& executable,
                             std::span<const std::string> arguments,
                             std::chrono::milliseconds timeout) override {
      require(std::filesystem::is_regular_file(executable) && timeout.count() > 0, "process configuration");
      calls.emplace_back(arguments.begin(), arguments.end());
      if (arguments[0] == "-l") {
         return {0, false, "unparsed vendor listing"};
      }
      require(arguments.size() == 2, "no shell or unmount-all invocation");
      const auto cue = flaccd::pathFromUtf8(arguments[1]);
      require(cue.is_absolute(), "ImgDrive must receive absolute CUE path");
      flaccd::MountStateStore store(cache);
      const auto records = store.records();
      const auto found = std::find_if(records.begin(), records.end(), [&](const auto& record) {
         return record.cuePath == cue;
      });
      require(found != records.end(), "persistent state must precede invocation");
      const bool unmount = arguments[0] == "-u";
      require(found->state == (unmount ? flaccd::MountState::Unmounting : flaccd::MountState::Mounting) ||
              (unmount && found->state == flaccd::MountState::Detached), "incorrect persisted phase");
      if (!unmount) {
         require(arguments[0] == "-m:" + std::string(1, found->drive), "explicit drive mount syntax");
         const auto parsed = flaccd::parseCueTextMetadata(readFile(cue));
         const auto reference = flaccd::pathFromUtf8(parsed.fileName);
         const auto audio = reference.is_absolute() ? reference : cue.parent_path() / reference;
         require(parsed.errors.empty() && parsed.fileType == "WAVE", "valid WAVE CUE required");
         require(cue.stem() == audio.stem() && cue.parent_path() == audio.parent_path(),
                 "ImgDrive requires matching basenames and same directory");
         require(std::filesystem::equivalent(audio, found->audioPath), "wrong audio resource");
         require(readFile(audio).starts_with("fLaC"), "mock mounter cannot read FLAC");
         if (onMount) {
            onMount();
         }
         pendingCue = cue;
         pendingDrive = redirectDrive ? redirectDrive : found->drive;
      }
      if (result.exitCode == 0 && !result.timedOut && changeMedia) {
         media[unmount ? found->actualDrive : (redirectDrive ? redirectDrive : found->drive)] = unmount ? flaccd::MediaState::Empty : flaccd::MediaState::Ready;
      }
      return result;
   }
   flaccd::MediaState inspectDrive(char drive) override {
      return media.contains(drive) ? media.at(drive) : flaccd::MediaState::Empty;
   }
   std::vector<flaccd::OpticalDrive> opticalDrives() override {
      std::vector<flaccd::OpticalDrive> drives;
      for (char drive = 'D'; drive <= 'F'; ++drive) {
         if (!missing.contains(drive)) {
            drives.push_back({drive, "mock-device-" + std::string(1, drive) + identitySuffix, "test",
                              !physical.contains(drive), inspectDrive(drive)});
         }
      }
      return drives;
   }
   bool matchesLayout(char, const flaccd::TrackLayout&) override {
      return layoutMatches;
   }
   std::optional<std::vector<char>> emptyDrives() override {
      return available;
   }
   std::unique_ptr<flaccd::MountLock> lock(const std::filesystem::path&, std::chrono::milliseconds) override {
      return std::make_unique<TestLock>(mutex);
   }
   void pause(std::chrono::milliseconds) override {
      if (readyAfterPauses > 0) {
         require(std::filesystem::exists(pendingCue), "CUE deleted after process exit");
         const auto parsed = flaccd::parseCueTextMetadata(readFile(pendingCue));
         require(std::filesystem::is_regular_file(pendingCue.parent_path() / flaccd::pathFromUtf8(parsed.fileName)),
                 "audio deleted during delayed completion");
         if (--readyAfterPauses == 0) {
            media[pendingDrive] = flaccd::MediaState::Ready;
         }
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
   }
   std::filesystem::path cache;
   std::recursive_mutex mutex;
   flaccd::ProcessResult result;
   std::function<void()> onMount;
   char redirectDrive = 0;
   std::string identitySuffix;
   std::set<char> physical;
   std::set<char> missing;
   bool changeMedia = true;
   bool layoutMatches = true;
   int readyAfterPauses = 0;
   char pendingDrive = 0;
   std::filesystem::path pendingCue;
   std::map<char, flaccd::MediaState> media;
   std::optional<std::vector<char>> available;
   std::vector<std::vector<std::string>> calls;
};

class Scenario {
public:
   explicit Scenario(const std::filesystem::path& fixtures) : directory(fixtures) {
      source = directory.path / flaccd::pathFromUtf8("Zelda album 音楽 é.flac");
      std::filesystem::copy_file(fixtures / "fixture-4096.flac", source);
      original = readFile(source);
      executable = directory.path / "ImgDrive mock.exe";
      writeFile(executable, "MZmock");
      host.cache = directory.path / flaccd::pathFromUtf8("cache 音楽 & spaces");
      files.adjacent = source;
      files.adjacent.replace_extension(".cue");
      metadata = flaccd::FlacMetadataInspector{}.inspect(source);
   }
   std::unique_ptr<flaccd::ImgDriveBackend> backend(std::chrono::milliseconds timeout = std::chrono::seconds(1)) {
      return std::make_unique<flaccd::ImgDriveBackend>(host, executable, host.cache, timeout, &files,
                                                      [&](const std::string& message) { logs.push_back(message); });
   }
   std::string cue() const {
      return flaccd::generateRelativeFlacCueSheet(flaccd::selectTrackLayout(metadata, "auto"), metadata,
                                                flaccd::pathToUtf8(source.filename()));
   }
   void unchanged() const {
      require(readFile(source) == original, "original FLAC modified");
      for (const auto& entry : std::filesystem::recursive_directory_iterator(directory.path)) {
         require(entry.path().extension() != ".bin", "mount generated a raw BIN");
         require(!std::filesystem::is_symlink(entry.path()), "mount created a symlink");
      }
   }
   TestDirectory directory;
   std::filesystem::path source;
   std::filesystem::path executable;
   std::string original;
   flaccd::FlacMetadata metadata;
   TestFileSystem files;
   FakeHost host;
   std::vector<std::string> logs;
};

void verifyRecord(const Scenario& scenario, const flaccd::MountRecord& record) {
   const auto parsed = flaccd::parseCueTextMetadata(readFile(record.cuePath));
   require(flaccd::pathFromUtf8(parsed.fileName).is_relative(), "generated CUE must use relative FILE");
   require(parsed.fileName == flaccd::pathToUtf8(record.audioPath.filename()), "wrong FILE filename");
   require(record.cuePath.stem() == record.audioPath.stem(), "mismatching basenames");
   const auto expected = flaccd::selectTrackLayout(scenario.metadata, "auto");
   const auto actual = flaccd::TrackLayout::fromText(scenario.metadata, parsed);
   require(flaccd::compareTrackLayouts(expected, actual).empty(), "normalized track/index/gap layout changed");
   require(readFile(record.audioPath) == scenario.original, "staged encoded FLAC differs");
   for (const auto& track : expected.getTracks()) {
      for (const auto& index : track.indices) {
         for (std::int64_t delta = -1; delta <= 1; ++delta) {
            const auto before = expected.queryLba(index.discLba + delta);
            const auto after = actual.queryLba(index.discLba + delta);
            require(before.has_value() == after.has_value(), "index boundary presence differs");
            if (before) {
               require(before->sourceSector == after->sourceSector && before->indexNumber == after->indexNumber &&
                       before->trackNumber == after->trackNumber, "index boundary mapping differs");
            }
         }
      }
   }
}

void testAdjacent(const std::filesystem::path& fixtures) {
   Scenario s(fixtures);
   auto backend = s.backend();
   expectError([&] { backend->mount(s.source); }, "enumeration");
   const auto record = backend->mount(s.source, 'D', "auto", {}, true);
   require(record.location == flaccd::CueLocation::Adjacent && record.cuePath == s.files.adjacent,
           "writable source must use adjacent CUE");
   require(s.files.linkCalls == 0 && s.files.copyCalls == 0, "adjacent mount staged audio unnecessarily");
   verifyRecord(s, record);
   const auto calls = s.host.calls.size();
   expectError([&] { backend->mount(s.source, 'D'); }, "already has FlacCD state");
   expectError([&] { backend->unmount('E'); }, "no FlacCD-owned");
   require(s.host.calls.size() == calls, "unrelated/active drive invoked ImgDrive");
   backend.reset();
   require(std::filesystem::exists(record.cuePath), "CLI exit deleted adjacent CUE");
   backend = s.backend();
   backend->unmount('D');
   require(!std::filesystem::exists(record.cuePath) && backend->status().empty(), "adjacent cleanup failed");
   s.unchanged();
}

void testExisting(const std::filesystem::path& fixtures) {
   Scenario s(fixtures);
   auto text = s.cue();
   text.insert(0, "\xef\xbb\xbfREM harmless comment\r\n");
   writeFile(s.files.adjacent, text);
   const auto identity = s.files.identity(s.files.adjacent);
   auto backend = s.backend();
   const auto record = backend->mount(s.source, 'F', "auto", {}, true);
   require(record.location == flaccd::CueLocation::Existing, "equivalent CUE was not reused");
   require(s.files.linkCalls == 0 && s.files.copyCalls == 0, "existing equivalent CUE staged audio");
   backend->unmount('F');
   require(readFile(s.files.adjacent) == text && s.files.identity(s.files.adjacent) == identity,
           "user-owned CUE modified or deleted");
   s.unchanged();
}

void testStaging(const std::filesystem::path& fixtures) {
   for (int variant = 0; variant < 3; ++variant) {
      Scenario s(fixtures);
      if (variant == 0) {
         auto conflict = s.cue();
         conflict.replace(conflict.find("00:00:28"), 8, "00:00:29");
         writeFile(s.files.adjacent, conflict);
      } else {
         s.files.denyAdjacent = true;
      }
      if (variant == 2) {
         s.files.linkError = std::make_error_code(std::errc::cross_device_link);
      }
      const auto external = flaccd::mountPathExists(s.files.adjacent) ? readFile(s.files.adjacent) : std::string{};
      auto backend = s.backend();
      const auto record = backend->mount(s.source, 'D', "auto", {}, variant != 2);
      require(record.location == flaccd::CueLocation::Staged, "conflict/read-only directory did not stage");
      require(record.storage == (variant == 2 ? flaccd::AudioStorage::Copy : flaccd::AudioStorage::HardLink),
              "incorrect hard-link/copy preference");
      require(s.files.copyCalls == (variant == 2 ? 1 : 0), "unexpected compressed copy count");
      if (variant != 2) {
         require(std::filesystem::equivalent(record.audioPath, s.source), "staged hard link is not original inode");
      } else {
         require(!std::filesystem::equivalent(record.audioPath, s.source), "copy unexpectedly aliases original");
         require(std::any_of(s.logs.begin(), s.logs.end(), [&](const auto& text) {
            return text.find("Copying compressed FLAC") != std::string::npos &&
                   text.find(flaccd::pathToUtf8(s.source)) != std::string::npos &&
                   text.find(flaccd::pathToUtf8(record.audioPath)) != std::string::npos;
         }), "copy was not reported with source/destination");
      }
      verifyRecord(s, record);
      backend->unmount('D');
      require(!std::filesystem::exists(record.entryPath), "owned staged directory not cleaned");
      if (!external.empty()) {
         require(readFile(s.files.adjacent) == external, "conflicting external CUE modified/deleted");
      }
      s.unchanged();
   }
}

void testNoCopyAndErrors(const std::filesystem::path& fixtures) {
   Scenario s(fixtures);
   s.files.denyAdjacent = true;
   s.files.linkError = std::make_error_code(std::errc::operation_not_supported);
   auto backend = s.backend();
   expectError([&] { backend->mount(s.source, 'D', "auto", {}, true); }, "--no-copy");
   require(s.files.copyCalls == 0 && s.host.calls.empty() && backend->status().empty(), "--no-copy copied or mounted");
   s.files.linkError = std::make_error_code(std::errc::io_error);
   expectError([&] { backend->mount(s.source, 'D'); }, "hard link");
   require(s.files.copyCalls == 0 && s.host.calls.empty(), "unexpected hard-link failure was hidden by copying");
   s.unchanged();
   Scenario failed(fixtures);
   failed.files.denyAdjacent = true;
   failed.files.linkError = std::make_error_code(std::errc::cross_device_link);
   failed.files.failCopy = true;
   auto other = failed.backend();
   expectError([&] { other->mount(failed.source, 'D'); }, "copy failure");
   require(failed.host.calls.empty() && other->status().size() == 1 &&
           other->status()[0].record.state == flaccd::MountState::Preparing, "partial copy was mounted or lost its intent");
   failed.unchanged();
}

void testCreationRace(const std::filesystem::path& fixtures) {
   for (const bool conflict : {false, true}) {
      Scenario s(fixtures);
      s.files.race = true;
      s.files.racedCue = conflict ? "User's conflicting CUE\n" : s.cue();
      auto backend = s.backend();
      const auto record = backend->mount(s.source, 'D');
      require(record.location == (conflict ? flaccd::CueLocation::Staged : flaccd::CueLocation::Existing),
              "exclusive-create race did not re-evaluate the new CUE");
      backend->unmount('D');
      require(readFile(s.files.adjacent) == s.files.racedCue, "racing CUE overwritten or deleted");
      s.unchanged();
   }
}

void testUncertainAndRecovery(const std::filesystem::path& fixtures) {
   for (int variant = 0; variant < 4; ++variant) {
      Scenario s(fixtures);
      if (variant == 0) {
         s.host.result = {5, false, "vendor error"};
      } else if (variant == 1) {
         s.host.result = {259, true, "timeout"};
      } else if (variant == 2) {
         s.host.layoutMatches = false;
      } else {
         s.host.changeMedia = false;
      }
      auto backend = s.backend(std::chrono::milliseconds(2));
      expectError([&] { backend->mount(s.source, 'D'); });
      const auto record = backend->status()[0].record;
      require(record.state == flaccd::MountState::Mounting && std::filesystem::exists(record.cuePath),
              "uncertain mount lost CUE/state");
      backend.reset();
      s.host.result = {};
      s.host.changeMedia = true;
      s.host.layoutMatches = true;
      backend = s.backend();
      backend->unmount('D');
      require(backend->status().empty() && !std::filesystem::exists(record.cuePath), "stale/pending recovery failed");
      s.unchanged();
   }
   Scenario s(fixtures);
   s.files.denyAdjacent = true;
   auto backend = s.backend(std::chrono::milliseconds(2));
   const auto record = backend->mount(s.source, 'D');
   s.host.result = {7, false, "unmount denied"};
   expectError([&] { backend->unmount('D'); }, "unmount denied");
   require(std::filesystem::exists(record.cuePath) && std::filesystem::exists(record.audioPath), "failed unmount deleted resources");
   s.host.result = {};
   s.host.changeMedia = false;
   expectError([&] { backend->unmount('D'); }, "not confirmed");
   require(std::filesystem::exists(record.audioPath), "unconfirmed unmount deleted staged FLAC");
   s.host.changeMedia = true;
   backend.reset();
   backend = s.backend();
   backend->unmount('D');
   s.unchanged();
}

void testChangedAdjacent(const std::filesystem::path& fixtures) {
   for (const bool replace : {false, true}) {
      Scenario s(fixtures);
      auto backend = s.backend();
      const auto record = backend->mount(s.source, 'D');
      if (replace) {
         const auto replacement = s.directory.path / "replacement.cue";
         writeFile(replacement, record.cueText);
         std::filesystem::remove(record.cuePath);
         std::filesystem::rename(replacement, record.cuePath);
      } else {
         writeFile(record.cuePath, "User modified this CUE\n");
      }
      const auto keep = readFile(record.cuePath);
      expectError([&] { backend->unmount('D'); }, "replaced or modified");
      require(readFile(record.cuePath) == keep && backend->status()[0].record.state == flaccd::MountState::Detached,
              "modified/replaced adjacent CUE was deleted or detachment was not persisted");
      s.unchanged();
   }
}

void testConcurrency(const std::filesystem::path& fixtures) {
   for (const bool userOwned : {false, true}) {
      Scenario s(fixtures);
      const auto userCue = s.cue();
      if (userOwned) {
         writeFile(s.files.adjacent, userCue);
      }
      std::array<std::exception_ptr, 2> errors;
      std::array<std::thread, 2> threads;
      for (std::size_t position = 0; position < threads.size(); ++position) {
         threads[position] = std::thread([&, position] {
            try {
               auto backend = s.backend();
               backend->mount(s.source, position == 0 ? 'D' : 'E');
            } catch (...) {
               errors[position] = std::current_exception();
            }
         });
      }
      for (auto& thread : threads) {
         thread.join();
      }
      for (const auto& error : errors) {
         if (error) {
            std::rethrow_exception(error);
         }
      }
      auto backend = s.backend();
      const auto records = backend->status();
      require(records.size() == 2 && records[0].record.cuePath != records[1].record.cuePath,
              "concurrent requests shared a CUE path");
      const auto adjacent = std::find_if(records.begin(), records.end(), [](const auto& item) {
         return item.record.location != flaccd::CueLocation::Staged;
      });
      require(adjacent != records.end(), "first concurrent request unnecessarily staged");
      const char first = adjacent->record.drive;
      const char second = first == 'D' ? 'E' : 'D';
      backend->unmount(first);
      const auto remaining = backend->status();
      require(remaining.size() == 1 && std::filesystem::exists(remaining[0].record.cuePath) &&
              std::filesystem::exists(remaining[0].record.audioPath), "first unmount removed another mount's resources");
      backend->unmount(second);
      require(userOwned ? readFile(s.files.adjacent) == userCue : !std::filesystem::exists(s.files.adjacent),
              "concurrent ownership cleanup incorrect");
      s.unchanged();
   }
}

void testValidation(const std::filesystem::path& fixtures) {
   Scenario s(fixtures);
   auto backend = s.backend();
   expectError([&] { backend->mount(fixtures / "fixture-gap.flac", 'D', "text"); }, "source-less");
   expectError([&] { backend->mount(fixtures / "fixture-no-first-pregap.flac", 'D'); }, "conflict");
   expectError([&] { backend->mount(fixtures / "unsupported-0.flac", 'D'); }, "no valid");
   require(s.host.calls.empty() && backend->status().empty(), "invalid layout invoked ImgDrive");
   s.host.changeMedia = false;
   s.host.readyAfterPauses = 2;
   const auto record = backend->mount(s.source, 'D');
   require(record.state == flaccd::MountState::Mounted && s.host.readyAfterPauses == 0,
           "did not wait after asynchronous process exit");
   s.host.changeMedia = true;
   backend->unmount('D');
   const auto calls = s.host.calls.size();
   const auto moved = s.source.parent_path() / "temporarily moved.flac";
   expectError([&] {
      backend->mount(s.source, 'D', "auto", [&](const flaccd::MountRecord&) {
         std::filesystem::rename(s.source, moved);
      });
   });
   require(s.host.calls.size() == calls, "source disappearance reached ImgDrive");
   std::filesystem::rename(moved, s.source);
   require(backend->status()[0].record.state == flaccd::MountState::Prepared,
           "preflight failure lost recoverable ownership");
   backend->unmount('D');
   expectError([&] { flaccd::ImgDriveBackend bad(s.host, s.directory.path / "missing.exe", s.host.cache); }, "missing");
   writeFile(s.directory.path / "invalid.exe", "not PE");
   expectError([&] { flaccd::ImgDriveBackend bad(s.host, s.directory.path / "invalid.exe", s.host.cache); }, "invalid");
   require(flaccd::quoteWindowsArgument(L"ends\\") == L"\"ends\\\\\"", "production quoting regressed");
}

void testOwnedCacheProtection(const std::filesystem::path& fixtures) {
   Scenario s(fixtures);
   s.files.denyAdjacent = true;
   auto backend = s.backend();
   const auto record = backend->mount(s.source, 'D');
   writeFile(record.entryPath / "unrelated.txt", "Do not delete");
   expectError([&] { backend->unmount('D'); }, "unrecognized");
   require(std::filesystem::exists(record.audioPath) && readFile(record.entryPath / "unrelated.txt") == "Do not delete",
           "unexpected cache file caused unsafe cleanup");
   std::filesystem::remove(record.entryPath / "unrelated.txt");
   backend->unmount('D');
   s.unchanged();
}

void testCue(const std::filesystem::path& source) {
   const auto metadata = flaccd::FlacMetadataInspector{}.inspect(source);
   for (const auto selection : {"native", "text", "auto"}) {
      const auto original = flaccd::selectTrackLayout(metadata, selection);
      const auto cue = flaccd::generateFlacCueSheet(original, metadata, source);
      const auto parsed = flaccd::parseCueTextMetadata(cue);
      require(parsed.errors.empty() && parsed.fileType == "WAVE", "FLAC-backed FILE must use WAVE");
      require(std::filesystem::equivalent(flaccd::pathFromUtf8(parsed.fileName), source), "wrong FLAC referenced");
      require(flaccd::pathFromUtf8(parsed.fileName).is_absolute(), "absolute writer must use absolute FLAC reference");
#ifdef _WIN32
      // On Windows the generated string itself must be fully qualified, not
      // merely a drive-relative path that filesystem::equivalent can resolve.
      const auto& name = parsed.fileName;
      const bool drivePath = name.size() >= 3 && name[1] == ':' && (name[2] == '\\' || name[2] == '/');
      const bool uncPath = name.starts_with("\\\\") || name.starts_with("//");
      require(drivePath || uncPath, "generated FILE must use an absolute Windows drive/UNC path");
      require(!name.starts_with("\\\\?\\"), "CUE must not contain a Win32 extended-device path prefix");
      require(name.find(flaccd::pathToUtf8(source.filename())) != std::string::npos,
              "Windows FILE reference lost spaces or Unicode filename bytes");
#endif
      require(cue.find("INDEX 00 00:00:00") != std::string::npos &&
              cue.find("INDEX 01 00:00:02") != std::string::npos, "CUE lost pregap or added two seconds");
      require(parsed.title == "Fixture Album" && parsed.performer == "Fixture Album Artist", "descriptive metadata lost");
      const auto roundTrip = flaccd::TrackLayout::fromText(metadata, parsed);
      require(flaccd::compareTrackLayouts(original, roundTrip).empty(), "FLAC CUE round-trip differs");
      const auto relativeCue = flaccd::generateRelativeFlacCueSheet(original, metadata,
                                                                  flaccd::pathToUtf8(source.filename()));
      const auto relative = flaccd::parseCueTextMetadata(relativeCue);
      require(relative.fileName == flaccd::pathToUtf8(source.filename()) && relative.fileType == "WAVE" &&
              flaccd::compareTrackLayouts(original, flaccd::TrackLayout::fromText(metadata, relative)).empty(),
              "relative native/text CUE lost layout or Unicode filename");
      for (const auto unsafe : {"C:\\Music\\album.flac", "\\\\server\\share\\album.flac", "../album.flac",
                                "unsafe\"name.flac", "bad\nname.flac"}) {
         expectError([&] { flaccd::generateRelativeFlacCueSheet(original, metadata, unsafe); });
      }
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

void testQuoting() {
   require(flaccd::quoteWindowsArgument(L"C:\\Music with spaces\\音楽.flac") == L"\"C:\\Music with spaces\\音楽.flac\"",
           "Unicode/spaces command quoting");
   require(flaccd::quoteWindowsArgument(L"a\"b") == L"\"a\\\"b\"", "embedded quote escaping");
   require(flaccd::quoteWindowsArgument(L"end\\") == L"\"end\\\\\"", "trailing backslash escaping");
   require(flaccd::quoteWindowsArgument(L"&;|<>^") == L"\"&;|<>^\"", "shell characters must remain literal arguments");
   expectError([&] { flaccd::quoteWindowsArgument(std::wstring(L"bad\0argument", 12)); }, "NUL");
}


void testDriveMapping(const std::filesystem::path& fixtures) {
   {
      Scenario s(fixtures);
      s.host.physical.insert('D');
      auto backend = s.backend();
      expectError([&] { backend->mount(s.source, 'D'); }, "not an identified ImgDrive");
      require(s.host.calls.empty() && !std::filesystem::exists(s.files.adjacent), "physical drive received a mount");
      s.host.media['E'] = flaccd::MediaState::Ready;
      expectError([&] { backend->mount(s.source, 'E'); }, "occupied");
      require(s.host.calls.empty(), "occupied drive received a mount");
      s.host.media['E'] = flaccd::MediaState::Empty;
      s.host.available = std::vector<char>{'E'};
      const auto record = backend->mount(s.source);
      require(record.drive == 'E' && record.actualDrive == 'E' && record.imgDriveSelector == "E" &&
              record.deviceIdentity == "mock-device-E", "automatic drive binding wrong");
      const auto message = backend->unmount('E');
      require(message.find("removed generated CUE") != std::string::npos &&
              message.find("user CUE retained") == std::string::npos, "generated CUE cleanup misreported");
   }
   {
      Scenario s(fixtures);
      s.host.redirectDrive = 'E';
      auto backend = s.backend();
      expectError([&] { backend->mount(s.source, 'D'); }, "drive-selection error");
      const auto pending = backend->status();
      require(pending.size() == 1 && pending[0].record.drive == 'D' && pending[0].record.actualDrive == 'E' &&
              pending[0].record.deviceIdentity == "mock-device-E" && std::filesystem::exists(s.files.adjacent),
              "wrong-drive mount lost actual binding/resources");
      expectError([&] { backend->unmount('D'); }, "no FlacCD-owned");
      backend->unmount('E');
      require(s.host.media['E'] == flaccd::MediaState::Empty, "actual device did not unmount");
   }
   {
      Scenario s(fixtures);
      s.host.layoutMatches = false;
      auto backend = s.backend();
      expectError([&] { backend->mount(s.source, 'D'); }, "TOC mismatch");
      require(std::filesystem::exists(s.files.adjacent), "TOC mismatch deleted uncertain resources");
   }
   {
      Scenario s(fixtures);
      s.host.result = {7, false, "vendor returned before target was identified"};
      auto backend = s.backend(std::chrono::milliseconds(2));
      expectError([&] { backend->mount(s.source, 'D'); }, "failed");
      s.host.media['E'] = flaccd::MediaState::Ready;
      s.host.result = {};
      expectError([&] { backend->unmount('D'); }, "not confirmed");
      require(std::filesystem::exists(s.files.adjacent) && s.host.media['E'] == flaccd::MediaState::Ready,
              "empty requested drive permitted deletion while actual target was uncertain");
      s.host.media['E'] = flaccd::MediaState::Empty;
      backend->unmount('D');
   }

}

flaccd::AgentReply ipc(flaccd::MountAgent& agent, const std::vector<std::string>& arguments) {
   return flaccd::decodeAgentReply(flaccd::encodeAgentReply(agent.handle(
      flaccd::decodeAgentRequest(flaccd::encodeAgentRequest(arguments)))));
}

void testAgent(const std::filesystem::path& fixtures) {
   Scenario s(fixtures);
   auto agent = std::make_unique<flaccd::MountAgent>(s.host, s.executable, s.host.cache);
   auto mounted = ipc(*agent, {"mount", flaccd::pathToUtf8(s.source), "--drive", "D:", "--no-copy"});
   require(mounted.exitCode == 0 && mounted.records.size() == 1 && mounted.records[0][2] == "D",
           "IPC mount/status structure wrong");
   const auto now = std::chrono::steady_clock::now();
   s.host.media['D'] = flaccd::MediaState::Empty; // Explorer eject, no CLI unmount.
   require(agent->reconcile(now).empty() && std::filesystem::exists(s.files.adjacent), "single notification deleted CUE");
   s.host.media['D'] = flaccd::MediaState::Unknown;
   require(agent->reconcile(now + std::chrono::seconds(6)).empty() && std::filesystem::exists(s.files.adjacent),
           "uncertain media permitted cleanup");
   s.host.media['D'] = flaccd::MediaState::Empty;
   agent.reset();
   agent = std::make_unique<flaccd::MountAgent>(s.host, s.executable, s.host.cache);
   agent->reconcile(now + std::chrono::seconds(7));
   require(std::filesystem::exists(s.files.adjacent), "restart bypassed debounce");
   // No notification: periodic reconciliation alone must recover the eject.
   const auto cleanup = agent->reconcile(now + std::chrono::seconds(13));
   require(cleanup.find("removed generated CUE") != std::string::npos && !std::filesystem::exists(s.files.adjacent),
           "missed eject/restart recovery failed");
   require(ipc(*agent, {"status"}).records.empty(), "ejected record not removed");
   writeFile(s.files.adjacent, s.cue());
   require(ipc(*agent, {"mount", flaccd::pathToUtf8(s.source), "--drive", "E:"}).exitCode == 0, "agent remount failed");
   const auto unmount = ipc(*agent, {"unmount", "E:"});
   require(unmount.exitCode == 0 && unmount.output.find("user CUE retained") != std::string::npos &&
           std::filesystem::exists(s.files.adjacent), "agent removed/misreported user CUE");

   std::array<flaccd::AgentReply, 2> replies;
   std::thread first([&] { replies[0] = ipc(*agent, {"mount", flaccd::pathToUtf8(s.source), "--drive", "D:"}); });
   std::thread second([&] { replies[1] = ipc(*agent, {"mount", flaccd::pathToUtf8(s.source), "--drive", "E:"}); });
   first.join();
   second.join();
   require(replies[0].exitCode == 0 && replies[1].exitCode == 0, "concurrent agent clients failed");
   require(ipc(*agent, {"status"}).records.size() == 2, "concurrent IPC lost a mount");
   s.host.media['D'] = flaccd::MediaState::Empty;
   s.host.media['E'] = flaccd::MediaState::Empty;
   agent->reconcile(now + std::chrono::seconds(20));
   agent->reconcile(now + std::chrono::seconds(26));
   require(ipc(*agent, {"status"}).records.empty() && readFile(s.files.adjacent) == s.cue(),
           "concurrent eject cleanup deleted shared user resource");
   require(ipc(*agent, {"mount", "relative.flac"}).exitCode == 2, "relative IPC source accepted");
   s.unchanged();
}

void testAgentStop(const std::filesystem::path& fixtures) {
   for (int variant = 0; variant < 4; ++variant) {
      Scenario s(fixtures);
      if (variant == 1) {
         writeFile(s.files.adjacent, s.cue());
      } else if (variant >= 2) {
         s.files.denyAdjacent = true;
         if (variant == 3) {
            s.files.linkError = std::make_error_code(std::errc::cross_device_link);
         }
      }
      auto backend = s.backend();
      const auto record = backend->mount(s.source, 'D');
      const auto cueBytes = readFile(record.cuePath);
      const auto manifest = readFile(record.entryPath / "mount.state");
      backend.reset();
      const auto calls = s.host.calls.size();
      auto agent = std::make_unique<flaccd::MountAgent>(s.host, s.executable, s.host.cache);
      require(ipc(*agent, {"agent", "stop", "--timeout", "0"}).exitCode == 2 && !agent->isStopping(),
              "invalid stop changed agent lifecycle");
      const auto reply = ipc(*agent, {"agent", "stop"});
      require(reply.exitCode == 0 && agent->isStopping(), "agent stop was not accepted");
      require(ipc(*agent, {"agent-stop"}).exitCode == 0, "repeated legacy stop failed");
      require(ipc(*agent, {"mount", flaccd::pathToUtf8(s.source), "--drive", "E:"}).exitCode == 1,
              "stopping agent accepted a new mount");
      require(readFile(record.cuePath) == cueBytes && readFile(record.entryPath / "mount.state") == manifest &&
              readFile(record.audioPath) == s.original && s.host.calls.size() == calls,
              "shutdown unmounted or changed owned mount resources");
      const auto now = std::chrono::steady_clock::now();
      s.host.media['D'] = flaccd::MediaState::Empty; // Eject while monitoring is stopped.
      agent->reconcile(now);
      agent->reconcile(now + std::chrono::seconds(10));
      require(std::filesystem::exists(record.cuePath) && std::filesystem::exists(record.entryPath),
              "stopping agent reconciled/deleted resources");
      agent.reset();
      agent = std::make_unique<flaccd::MountAgent>(s.host, s.executable, s.host.cache);
      agent->reconcile(now + std::chrono::seconds(20));
      agent->reconcile(now + std::chrono::seconds(26));
      require(ipc(*agent, {"status"}).records.empty(), "restart did not recover eject while stopped");
      require(variant == 1 ? readFile(record.cuePath) == cueBytes : !std::filesystem::exists(record.cuePath),
              "restart cleanup mishandled user/generated CUE");
      s.unchanged();
   }
}

void testStopInProgress(const std::filesystem::path& fixtures) {
   Scenario s(fixtures);
   flaccd::MountAgent agent(s.host, s.executable, s.host.cache);
   std::promise<void> entered;
   std::promise<void> release;
   auto ready = entered.get_future();
   auto proceed = release.get_future().share();
   s.host.onMount = [&] { entered.set_value(); proceed.wait(); };
   flaccd::AgentReply mounted;
   flaccd::AgentReply stopped;
   flaccd::AgentReply rejected;
   std::thread mount([&] { mounted = ipc(agent, {"mount", flaccd::pathToUtf8(s.source), "--drive", "D:"}); });
   if (ready.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
      release.set_value();
      mount.join();
      require(false, "in-flight mount did not reach the mock decoder process");
   }
   std::thread stop([&] { stopped = ipc(agent, {"agent", "stop"}); });
   const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
   while (!agent.isStopping() && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
   }
   const bool announced = agent.isStopping();
   std::thread queued([&] { rejected = ipc(agent, {"mount", flaccd::pathToUtf8(s.source), "--drive", "E:"}); });
   release.set_value();
   mount.join();
   stop.join();
   queued.join();
   require(announced && mounted.exitCode == 0 && stopped.exitCode == 0 && rejected.exitCode == 1 &&
           rejected.diagnostics.find("stopping") != std::string::npos && s.host.calls.size() == 1,
           "shutdown failed to finish in-flight work/reject queued work");
   require(std::filesystem::exists(s.files.adjacent) && s.host.media['D'] == flaccd::MediaState::Ready,
           "in-flight shutdown discarded a mounted image");
   s.host.onMount = {};
   auto backend = s.backend();
   backend->unmount('D');
}

void testAgentUncertain(const std::filesystem::path& fixtures) {
   for (int variant = 0; variant < 5; ++variant) {
      Scenario s(fixtures);
      flaccd::MountAgent agent(s.host, s.executable, s.host.cache);
      if (variant == 0) {
         s.host.result = {7, false, "uncertain launch"};
      }
      const auto reply = ipc(agent, {"mount", flaccd::pathToUtf8(s.source), "--drive", "D:"});
      require((variant == 0) == (reply.exitCode != 0), "unexpected initial agent mount result");
      if (variant == 1) {
         s.host.identitySuffix = "-replaced";
      } else if (variant == 2) {
         s.host.missing.insert('D');
      } else if (variant == 3) {
         writeFile(s.files.adjacent, "replaced user contents");
      } else if (variant == 4) {
         s.host.result = {7, false, "uncertain unmount"};
         require(ipc(agent, {"unmount", "D:"}).exitCode == 1, "unmount failure not reported");
      }
      s.host.media['D'] = flaccd::MediaState::Empty;
      const auto now = std::chrono::steady_clock::now();
      agent.reconcile(now);
      agent.reconcile(now + std::chrono::seconds(6));
      require(std::filesystem::exists(s.files.adjacent) && !ipc(agent, {"status"}).records.empty(),
              "uncertain device/mount/ownership was cleaned");
   }
}

void testProtocol() {
   require(flaccd::normalizeAgentCommand({"agent", "stop"}) == std::vector<std::string>{"agent-stop"},
           "agent stop normalization");
   require(flaccd::agentStopTimeout({"agent-stop"}) == std::chrono::seconds(30), "default stop timeout");
   require(flaccd::agentStopTimeout({"agent-stop", "--timeout", "2"}) == std::chrono::seconds(2), "explicit stop timeout");
   expectError([&] { flaccd::normalizeAgentCommand({"agent"}); });
   expectError([&] { flaccd::normalizeAgentCommand({"agent", "unknown"}); });
   expectError([&] { flaccd::agentStopTimeout({"agent-stop", "--unknown", "1"}); });
   expectError([&] { flaccd::agentStopTimeout({"agent-stop", "--timeout", "1", "--timeout", "2"}); });
   const std::vector<std::string> arguments{"mount", "Y:\\Music with spaces\\音楽 é.flac", "--no-copy"};
   const auto text = flaccd::encodeAgentRequest(arguments);
   require(flaccd::decodeAgentRequest(text) == arguments, "IPC lost Windows/Unicode arguments");
   for (std::size_t size = 0; size < text.size(); ++size) {
      expectError([&] { flaccd::decodeAgentRequest(std::string_view(text).substr(0, size)); });
   }
   expectError([&] { flaccd::decodeAgentRequest(text + "extra"); }, "trailing");
   expectError([&] { flaccd::decodeAgentRequest(flaccd::encodeAgentRequest({std::string("a\0b", 3)})); }, "NUL");
   expectError([&] { flaccd::encodeAgentRequest({std::string(flaccd::MAX_AGENT_MESSAGE, 'a')}); }, "large");
   flaccd::AgentReply reply{2, "output", "diagnostic", {}};
   const auto result = flaccd::decodeAgentReply(flaccd::encodeAgentReply(reply));
   require(result.exitCode == 2 && result.output == reply.output && result.diagnostics == reply.diagnostics,
           "IPC error reply changed");
}

void testLegacyAndStateValidation(const std::filesystem::path& fixtures) {
   Scenario s(fixtures);
   flaccd::MountStateStore store(s.host.cache);
   const auto record = store.create('D', s.source, s.cue());
   auto forged = record;
   forged.entryPath = s.directory.path / "unrelated";
   expectError([&] { store.mark(forged, flaccd::MountState::Detached); }, "does not match");
   forged = record;
   forged.cuePath = s.source;
   expectError([&] { store.removeDetached(forged); }, "does not match");
   forged = record;
   forged.id = "../unrelated";
   expectError([&] { store.removeDetached(forged); }, "invalid mount ID");
   expectError([&] { store.removeDetached(record); }, "confirmed detachment");
   require(std::filesystem::exists(record.cuePath), "forged record changed owned CUE");

   // Upgrade must still allow image-specific unmount of a prior cache-only
   // mount. Old manifests never owned adjacent CUEs or staged audio files.
   std::ostringstream manifest;
   manifest << "FlacCD ImgDrive mount v1\n" << record.id << ' ' << record.drive << '\n'
            << std::quoted(flaccd::pathToUtf8(record.sourcePath)) << '\n'
            << std::quoted(record.cueText) << '\n';
   writeFile(record.entryPath / "mount.state", manifest.str());
   std::filesystem::remove(record.entryPath / "resources.state");
   writeFile(record.entryPath / "mounted", record.id);
   {
      auto backend = s.backend();
      const auto status = backend->status();
      require(status.size() == 1 && status[0].record.location == flaccd::CueLocation::Legacy &&
              status[0].record.state == flaccd::MountState::Mounted, "legacy state recovery failed");
      expectError([&] { backend->unmount('D'); }, "legacy/unverified");
   }
   store.mark(record, flaccd::MountState::Detached);
   store.removeDetached(record);
   require(!std::filesystem::exists(record.entryPath), "legacy mount cleanup failed");
   s.unchanged();

   const auto incomplete = store.begin('E', s.source, s.cue(), flaccd::CueLocation::Adjacent);
   writeFile(incomplete.entryPath / "mounted", incomplete.id);
   expectError([&] { store.records(); }, "partial or invalid");
   require(std::filesystem::exists(incomplete.entryPath / "mount.state"), "corrupt state was pruned");
}

}

int main(int argumentCount, char* arguments[]) {
   try {
      require(argumentCount == 2, "expected fixture directory");
      const auto fixtures = std::filesystem::absolute(arguments[1]);
      testQuoting();
      {
         Scenario s(fixtures);
         testCue(s.source);
      }
      testAdjacent(fixtures);
      testExisting(fixtures);
      testStaging(fixtures);
      testNoCopyAndErrors(fixtures);
      testCreationRace(fixtures);
      testUncertainAndRecovery(fixtures);
      testChangedAdjacent(fixtures);
      testConcurrency(fixtures);
      testValidation(fixtures);
      testOwnedCacheProtection(fixtures);
      testLegacyAndStateValidation(fixtures);
      testDriveMapping(fixtures);
      testProtocol();
      testAgent(fixtures);
      testAgentUncertain(fixtures);
      testAgentStop(fixtures);
      testStopInProgress(fixtures);
      std::cout << "ImgDrive strategy, ownership, recovery, race, and filesystem-fallback tests passed\n";
   } catch (const std::exception& error) {
      std::cerr << error.what() << '\n';
      return 1;
   }
   return 0;
}
