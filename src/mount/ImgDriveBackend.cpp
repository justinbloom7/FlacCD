#include "flaccd/mount/ImgDriveBackend.h"
#include "flaccd/CueSheetWriter.h"
#include "flaccd/PathText.h"

#include <fstream>
#include <iterator>
#include <stdexcept>

namespace flaccd {
namespace {

std::filesystem::path validateExecutable(const std::filesystem::path& path) {
   if (!std::filesystem::is_regular_file(path)) {
      throw std::invalid_argument("ImgDrive executable missing; configure --imgdrive with its installed .exe path");
   }
   const auto canonical = std::filesystem::canonical(path);
   std::ifstream input(canonical, std::ios::binary);
   char signature[2]{};
   input.read(signature, 2);
   if (!input || signature[0] != 'M' || signature[1] != 'Z') {
      throw std::invalid_argument("invalid ImgDrive executable: not a Windows executable");
   }
   // Full PE validation belongs to CreateProcessW; MZ is only an early check.
   return canonical;
}

std::chrono::milliseconds validateTimeout(std::chrono::milliseconds timeout) {
   if (timeout <= std::chrono::milliseconds::zero() || timeout > std::chrono::minutes(10)) {
      throw std::invalid_argument("timeout must be greater than zero and no more than ten minutes");
   }
   return timeout;
}

void validateResources(const MountRecord& record,
                       const FlacMetadata& metadata,
                       const TrackLayout& layout,
                       MountFileSystem& files) {
   if (readMountCue(record.cuePath) != record.cueText || files.identity(record.cuePath) != record.cueIdentity ||
       files.identity(record.audioPath) != record.audioIdentity) {
      throw std::runtime_error("mount resources changed before invocation; preserved");
   }
   const auto parsed = parseCueTextMetadata(record.cueText);
   const auto path = pathFromUtf8(parsed.fileName);
   const auto reference = path.is_absolute() ? path : record.cuePath.parent_path() / path;
   if (parsed.fileType != "WAVE" || !compareTrackLayouts(layout, TrackLayout::fromText(metadata, parsed)).empty() ||
       !std::filesystem::equivalent(reference, record.audioPath) ||
       record.cuePath.stem() != record.audioPath.stem() || record.cuePath.parent_path() != record.audioPath.parent_path()) {
      throw std::runtime_error("ImgDrive requires a matching adjacent CUE/FLAC pair with equivalent layout");
   }
   std::ifstream audio(record.audioPath, std::ios::binary);
   char signature[4]{};
   audio.read(signature, sizeof(signature));
   if (!audio || std::string_view(signature, sizeof(signature)) != "fLaC") {
      throw std::runtime_error("selected FLAC resource is unreadable or incomplete");
   }
}

bool samePath(const std::filesystem::path& first, const std::filesystem::path& second) {
   return std::filesystem::weakly_canonical(first) == std::filesystem::weakly_canonical(second) ||
          (mountPathExists(first) && mountPathExists(second) && std::filesystem::equivalent(first, second));
}

bool compatibleCue(const std::filesystem::path& cuePath,
                   const std::filesystem::path& source,
                   const FlacMetadata& metadata,
                   const TrackLayout& layout,
                   std::string& contents,
                   std::string& reason) {
   try {
      contents = readMountCue(cuePath);
      // Do not borrow a generated CUE from another active mount/cache. A separate
      // staging pair avoids deleting shared resources or unmounting both by name.
      if (contents.find("REM FlacCD ImgDrive mount ") != std::string::npos) {
         reason = "CUE is managed by another FlacCD mount";
         return false;
      }
      const auto parsed = parseCueTextMetadata(contents);
      const auto reference = pathFromUtf8(parsed.fileName);
      const auto audio = reference.is_absolute() ? reference : cuePath.parent_path() / reference;
      if (parsed.fileType != "WAVE" || cuePath.stem() != source.stem() ||
          !std::filesystem::equivalent(audio, source)) {
         reason = "FILE reference/type or matching basename does not identify the intended FLAC";
         return false;
      }
      const auto differences = compareTrackLayouts(layout, TrackLayout::fromText(metadata, parsed));
      if (!differences.empty()) {
         reason = differences.front();
         return false;
      }
      return true;
   } catch (const std::runtime_error& error) {
      reason = error.what();
   } catch (const std::invalid_argument& error) {
      reason = error.what();
   }
   return false;
}

char normalizeDrive(char drive) {
   if (drive >= 'a' && drive <= 'z') {
      drive = static_cast<char>(drive - 'a' + 'A');
   }
   if (drive < 'D' || drive > 'Z') {
      throw std::invalid_argument("ImgDrive drive letter must be D: through Z:");
   }
   return drive;
}

}

ImgDriveBackend::ImgDriveBackend(ProcessRunner& host,
                                 const std::filesystem::path& executablePath,
                                 const std::filesystem::path& cacheRoot,
                                 std::chrono::milliseconds operationTimeout,
                                 MountFileSystem* fileSystem,
                                 MountLog logger)
   : runner(host), executable(validateExecutable(executablePath)), timeout(validateTimeout(operationTimeout)),
     files(fileSystem ? *fileSystem : nativeFiles), log(std::move(logger)),
     guard(runner.lock(cacheRoot, timeout)), store(cacheRoot, &files) {
   if (!guard) {
      throw std::runtime_error("mount-state lock was not acquired");
   }
}

ProcessResult ImgDriveBackend::invoke(std::vector<std::string> arguments) {
   const auto result = runner.run(executable, arguments, timeout);
   if (result.timedOut || result.exitCode != 0) {
      throw std::runtime_error(std::string("ImgDrive ") + (result.timedOut ? "timed out" : "failed") +
                               " (exit " + std::to_string(result.exitCode) + "): " + result.diagnostics +
                               "; pending mount state/CUE retained if the command may have reached ImgDrive");
   }
   return result;
}

MountRecord ImgDriveBackend::mount(const std::filesystem::path& source,
                                   std::optional<char> requestedDrive,
                                   std::string_view layoutSource,
                                   const MountPrepared& prepared,
                                   bool noCopy) {
   char drive = 0;
   if (requestedDrive) {
      drive = normalizeDrive(*requestedDrive);
   } else {
      const auto available = runner.emptyDrives();
      if (!available) {
         throw std::runtime_error("automatic ImgDrive enumeration is not verified for this CLI; "
                                  "run flaccd drives and specify --drive D:");
      }
      for (const auto candidate : *available) {
         if (runner.inspectDrive(normalizeDrive(candidate)) == MediaState::Empty) {
            drive = normalizeDrive(candidate);
            break;
         }
      }
      if (drive == 0) {
         throw std::runtime_error("no available ImgDrive virtual drive");
      }
   }
   for (const auto& record : store.records()) {
      if (record.drive == drive) {
         throw std::runtime_error("drive already has FlacCD state (" + mountStateName(record.state) +
                                  "); use flaccd status or flaccd unmount before remounting");
      }
   }
   if (runner.inspectDrive(drive) != MediaState::Empty) {
      throw std::runtime_error("selected optical drive is occupied, missing, or cannot be verified empty; "
                               "configure an empty visible ImgDrive drive first");
   }
   const auto original = std::filesystem::canonical(source);
   const auto metadata = FlacMetadataInspector{}.inspect(original);
   const auto layout = selectTrackLayout(metadata, layoutSource);
   const auto record = prepare(drive, original, metadata, layout, noCopy);
   store.validate(record);
   validateResources(record, metadata, layout, files);
   if (prepared) {
      prepared(record);
      store.validate(record);
      validateResources(record, metadata, layout, files);
   }
   if (runner.inspectDrive(drive) != MediaState::Empty) {
      throw std::runtime_error("drive changed during preparation; resources retained for recovery");
   }
   store.mark(record, MountState::Mounting);
   const auto result = invoke({"-m:" + std::string(1, drive), pathToUtf8(record.cuePath)});
   const auto deadline = std::chrono::steady_clock::now() + timeout;
   do {
      if (runner.inspectDrive(drive) == MediaState::Ready && runner.matchesLayout(drive, layout)) {
         store.mark(record, MountState::Mounted);
         auto mounted = record;
         mounted.state = MountState::Mounted;
         return mounted;
      }
      runner.pause(std::chrono::milliseconds(50));
   } while (std::chrono::steady_clock::now() < deadline);
   throw std::runtime_error("mount not confirmed: optical drive/TOC did not match before timeout; "
                            "CUE and mounting state retained; use flaccd status/unmount; ImgDrive output: " + result.diagnostics);
}

MountRecord ImgDriveBackend::prepare(char drive,
                                     const std::filesystem::path& source,
                                     const FlacMetadata& metadata,
                                     const TrackLayout& layout,
                                     bool noCopy) {
   auto extension = pathToUtf8(source.extension());
   for (auto& character : extension) {
      if (character >= 'A' && character <= 'Z') {
         character = static_cast<char>(character - 'A' + 'a');
      }
   }
   if (extension != ".flac") {
      throw std::invalid_argument("ImgDrive staging requires a .flac filename");
   }
   const auto generated = generateRelativeFlacCueSheet(layout, metadata, pathToUtf8(source.filename()));
   auto adjacent = source;
   adjacent.replace_extension(".cue");
   bool inUse = false;
   for (const auto& existing : store.records()) {
      inUse = inUse || samePath(existing.cuePath, adjacent);
   }
   const auto reuse = [&]() -> std::optional<MountRecord> {
      std::string contents;
      std::string reason;
      if (!inUse && compatibleCue(adjacent, source, metadata, layout, contents, reason)) {
         const auto intent = store.begin(drive, source, contents, CueLocation::Existing);
         if (log) {
            log("Reusing equivalent external CUE unchanged: " + pathToUtf8(adjacent));
         }
         return store.complete(intent, AudioStorage::Original);
      }
      if (log) {
         log("External CUE conflict; preserving it and using isolated staging: " + pathToUtf8(adjacent) +
             "; " + (inUse ? "already tracked by another mount" : reason));
      }
      return std::nullopt;
   };
   if (mountPathExists(adjacent)) {
      if (const auto existing = reuse()) {
         return *existing;
      }
   } else if (!inUse) {
      const auto intent = store.begin(drive, source, generated, CueLocation::Adjacent);
      const auto error = files.createCueExclusive(intent.cuePath, intent.cueText);
      if (!error) {
         if (log) {
            log("Created adjacent CUE without copying audio: " + pathToUtf8(intent.cuePath));
         }
         return store.complete(intent, AudioStorage::Original);
      }
      // Exclusive-create failure created no file. Delete only the cache intent,
      // never the target that another process might have created in the race.
      store.discardEmptyPreparation(intent);
      if (error == std::errc::file_exists) {
         if (const auto existing = reuse()) {
            return *existing;
         }
      } else if (!expectedLinkFailure(error)) {
         throw std::filesystem::filesystem_error("create adjacent CUE", adjacent, error);
      } else if (log) {
         log("Cannot create adjacent CUE; using isolated staging: " + error.message());
      }
   }
   const auto intent = store.begin(drive, source, generated, CueLocation::Staged);
   auto storage = AudioStorage::HardLink;
   const auto error = files.hardLink(source, intent.audioPath);
   if (error) {
      if (!expectedLinkFailure(error)) {
         throw std::filesystem::filesystem_error("stage FLAC hard link", source, intent.audioPath, error);
      }
      if (noCopy) {
         store.discardEmptyPreparation(intent);
         throw std::runtime_error("--no-copy: no compatible reusable/adjacent CUE and hard-link staging failed: " + error.message());
      }
      if (log) {
         log("Hard-link staging unavailable: " + error.message());
      }
      storage = AudioStorage::Copy;
      files.copyCompressed(source, intent.audioPath, log);
   } else if (log) {
      log("Staged FLAC hard link (no audio copy): " + pathToUtf8(intent.audioPath));
   }
   if (std::filesystem::file_size(intent.audioPath) != std::filesystem::file_size(source)) {
      throw std::runtime_error("staged FLAC has wrong size; incomplete resources retained, never mounted");
   }
   const auto cueError = files.createCueExclusive(intent.cuePath, intent.cueText);
   if (cueError) {
      throw std::system_error(cueError, "create staged matching CUE");
   }
   return store.complete(intent, storage);
}

void ImgDriveBackend::unmount(char requestedDrive) {
   const auto drive = normalizeDrive(requestedDrive);
   std::optional<MountRecord> selected;
   for (const auto& record : store.records()) {
      if (record.drive == drive) {
         if (selected) {
            throw std::runtime_error("ambiguous mount records; refusing to unmount");
         }
         selected = record;
      }
   }
   if (!selected) {
      throw std::runtime_error("no FlacCD-owned mount for this drive; unrelated media will not be ejected");
   }
   const auto& record = *selected;
   store.validate(record);
   if (record.state == MountState::Preparing) {
      throw std::runtime_error("preparation never reached ImgDrive; incomplete resources preserved for ownership inspection");
   }
   store.mark(record, MountState::Unmounting);
   // Vendor-documented image-specific unmount, never bare -u or -u:D.
   const auto result = invoke({"-u", pathToUtf8(record.cuePath)});
   const auto deadline = std::chrono::steady_clock::now() + timeout;
   do {
      const auto media = runner.inspectDrive(drive);
      if (media == MediaState::Empty || media == MediaState::Missing) {
         store.mark(record, MountState::Detached);
         store.removeDetached(record);
         return;
      }
      runner.pause(std::chrono::milliseconds(50));
   } while (std::chrono::steady_clock::now() < deadline);
   throw std::runtime_error("unmount not confirmed: drive still has media or status is unknown; "
                            "CUE/state retained; ImgDrive output: " + result.diagnostics);
}

std::vector<MountStatus> ImgDriveBackend::status() {
   std::vector<MountStatus> result;
   for (const auto& record : store.records()) {
      result.push_back({record, runner.inspectDrive(record.drive)});
   }
   return result;
}

std::string ImgDriveBackend::drives() {
   return invoke({"-l"}).diagnostics;
}

}
