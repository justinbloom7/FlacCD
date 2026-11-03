#include "flaccd/mount/ImgDriveBackend.h"
#include "flaccd/mount/MountCueDiscovery.h"
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
      const auto differences = compareTrackLayouts(layout, externalMountLayout(metadata, parsed, cuePath, source));
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
                                 MountLog logger,
                                 MountDetached onDetached)
   : runner(host), executable(validateExecutable(executablePath)), timeout(validateTimeout(operationTimeout)),
     files(fileSystem ? *fileSystem : nativeFiles), log(std::move(logger)), detached(std::move(onDetached)),
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
   const auto recovered = reconcileMissingMounts();
   if (log && !recovered.empty()) {
      log(recovered);
   }
   const auto original = std::filesystem::canonical(source);
   const auto metadata = FlacMetadataInspector{}.inspect(original);
   const auto layout = discoverMountLayout(metadata, original, layoutSource);
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
   const auto before = runner.opticalDrives();
   if (log) {
      log("Optical drives before mount:\n" + describeDrives(before));
   }
   const auto* target = findDrive(before, drive);
   if (!target || !target->imgDrive || target->identity.empty()) {
      throw std::runtime_error("requested drive " + std::string(1, drive) +
                               ": is not an identified ImgDrive device; select an empty ImgDrive drive:\n" + describeDrives(before));
   }
   for (const auto& record : store.records()) {
      if (record.drive == drive || record.actualDrive == drive || record.deviceIdentity == target->identity) {
         const auto* actual = findDevice(before, record.deviceIdentity);
         if (!actual || !actual->imgDrive || actual->media != MediaState::Ready) {
            auto reason = recoveryDiagnostic(record);
            if (reason.empty()) {
               reason = "media absence confirmed but owned resources could not be cleaned; inspect recovery diagnostics";
            }
            throw std::runtime_error("saved FlacCD mount is not proof of live media; recovery deferred: " +
                                     reason + "; CUE/state retained; inspect flaccd status");
         }
         std::string association = "live media association could not be verified";
         try {
            const auto savedMetadata = FlacMetadataInspector{}.inspect(record.audioPath);
            const auto savedLayout = TrackLayout::fromText(savedMetadata, parseCueTextMetadata(record.cueText));
            association = runner.matchesLayout(actual->letter, savedLayout) ?
               "live media TOC matches (TOC alone does not uniquely identify an image)" :
               "different or unverified image is present; " + runner.layoutDiagnostic();
         } catch (const std::exception& error) {
            association += ": " + std::string(error.what());
         }
         throw std::runtime_error("drive already has FlacCD state (" + mountStateName(record.state) +
                                  "); " + association + "; resources retained; inspect flaccd status");
      }
   }
   if (runner.inspectDrive(drive) != MediaState::Empty) {
      throw std::runtime_error("selected optical drive is occupied, missing, or cannot be verified empty; "
                               "configure an empty visible ImgDrive drive first");
   }
   auto record = prepare(drive, original, metadata, layout, noCopy);
   record = store.bindDevice(record, drive, target->identity, target->selector());
   store.validate(record);
   validateResources(record, metadata, layout, files);
   if (prepared) {
      prepared(record);
      store.validate(record);
      validateResources(record, metadata, layout, files);
   }
   const auto preflight = runner.opticalDrives();
   const auto* selected = findDrive(preflight, drive);
   if (!selected || selected->identity != target->identity || !selected->imgDrive || selected->media != MediaState::Empty) {
      throw std::runtime_error("drive identity/media changed during preparation; resources retained for recovery");
   }
   store.mark(record, MountState::Mounting);
   if (log) {
      log("Mount plan: requested=" + std::string(1, drive) + ": identity=" + record.deviceIdentity +
          " selector=-m:" + record.imgDriveSelector + "\nCUE: " + pathToUtf8(record.cuePath) +
          "\nAudio: " + pathToUtf8(record.audioPath));
   }
   const auto result = invoke({"-m:" + record.imgDriveSelector, pathToUtf8(record.cuePath)});
   const auto deadline = std::chrono::steady_clock::now() + timeout;
   unsigned mismatchCount = 0;
   bool readySeen = false;
   std::string tocDiagnostic;
   do {
      const auto after = runner.opticalDrives();
      const auto* actual = findDevice(after, record.deviceIdentity);
      if (actual && actual->imgDrive && actual->media == MediaState::Ready) {
         readySeen = true;
         if (runner.matchesLayout(actual->letter, layout)) {
            if (actual->letter != drive) {
               store.bindDevice(record, actual->letter, actual->identity, actual->selector());
               throw std::runtime_error("drive-selection error: requested " + std::string(1, drive) +
                                        ": but device is now " + actual->letter + ":; resources retained; use unmount " +
                                        actual->letter + ":\n" + describeDrives(after));
            }
            record = store.bindDevice(record, actual->letter, actual->identity, actual->selector());
            store.mark(record, MountState::Mounted);
            record.state = MountState::Mounted;
            if (log) {
               log("Optical drives after confirmed mount:\n" + describeDrives(after));
            }
            return record;
         }
         tocDiagnostic = runner.layoutDiagnostic();
         ++mismatchCount;
      }
      for (const auto& other : after) {
         const auto* prior = findDevice(before, other.identity);
         if (other.letter != drive && other.imgDrive && other.media == MediaState::Ready &&
             prior && prior->media == MediaState::Empty && runner.matchesLayout(other.letter, layout)) {
            store.bindDevice(record, other.letter, other.identity, other.selector());
            throw std::runtime_error("drive-selection error: requested " + std::string(1, drive) +
                                     ":; expected TOC appeared on " + other.letter +
                                     ": instead. No success reported; resources retained. Use unmount " + other.letter +
                                     ": after inspecting status.\n" + describeDrives(after));
         }
      }
      if (mismatchCount >= 3) {
         throw std::runtime_error("mount verification: media ready but audio-CD TOC mismatch; resources retained. " +
                                  tocDiagnostic + "\n" + describeDrives(after));
      }
      runner.pause(std::chrono::milliseconds(100));
   } while (std::chrono::steady_clock::now() < deadline);
   throw std::runtime_error(std::string("mount not confirmed: ") + (readySeen ? "TOC unavailable/mismatched" : "media never became ready") +
                            "; CUE/state retained; ImgDrive output: " + result.diagnostics +
                            "\nOptical drives after mount:\n" + describeDrives(runner.opticalDrives()));
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
      log("Staged FLAC hard link (no audio copy): " + pathToUtf8(source) + " -> " + pathToUtf8(intent.audioPath));
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

std::string ImgDriveBackend::recoveryDiagnostic(const MountRecord& record) {
   if (!record.resourcesReady || !record.deviceConfirmed ||
       (record.state != MountState::Mounted && record.state != MountState::Unmounting && record.state != MountState::Detached)) {
      return "mount/preparation was never confirmed; an interrupted vendor operation may still be pending";
   }
   const auto drives = runner.opticalDrives();
   const auto* actual = findDevice(drives, record.deviceIdentity);
   if (!actual || !actual->imgDrive || record.deviceIdentity.empty()) {
      return "recorded ImgDrive device is unavailable or its identity changed (unknown, not confirmed missing)";
   }
   if (actual->media != MediaState::Empty) {
      return actual->media == MediaState::Ready ? "recorded device contains live media" :
                                                "recorded device media state is unknown";
   }
   for (const auto& other : drives) {
      if (other.identity.empty() || other.media == MediaState::Unknown || other.media == MediaState::Missing ||
          (other.identity != record.deviceIdentity && other.imgDrive && other.media != MediaState::Empty)) {
         return "another virtual or uncertain optical drive might still use the image; cleanup deferred";
      }
   }
   return {};
}

std::string ImgDriveBackend::reconcileMissingMounts() {
   std::vector<MountRecord> candidates;
   for (const auto& record : store.records()) {
      if (recoveryDiagnostic(record).empty()) {
         candidates.push_back(record);
      }
   }
   std::string result;
   if (!candidates.empty()) {
      // Serialized by the cache lock. The real host waits; mocks can simulate
      // a media change/readiness transition between independent observations.
      runner.pause(std::chrono::milliseconds(500));
      for (const auto& record : candidates) {
         const auto diagnostic = recoveryDiagnostic(record);
         if (!diagnostic.empty()) {
            result += "Recovery deferred: " + diagnostic + "\n";
            continue;
         }
         try {
            result += "Recovered stale mount " + record.id + ":\n" + cleanupEjected(record, true);
         } catch (const std::exception& error) {
            result += "Recovery cleanup deferred; resources/state retained: " + std::string(error.what()) + "\n";
         }
      }
   }
   return result;
}

std::string ImgDriveBackend::cleanupEjected(const MountRecord& record, bool recovery) {
   store.validate(record);
   const auto drives = runner.opticalDrives();
   const auto* actual = findDevice(drives, record.deviceIdentity);
   if (!actual || !actual->imgDrive || actual->media != MediaState::Empty || record.deviceIdentity.empty()) {
      throw std::runtime_error("cleanup deferred: actual device is missing, changed, occupied, or uncertain");
   }
   if (record.state == MountState::Preparing || record.state == MountState::Prepared || record.state == MountState::Mounting) {
      throw std::runtime_error("cleanup deferred: mount was never confirmed; pending resources preserved");
   }
   if (recovery) {
      const auto diagnostic = recoveryDiagnostic(record);
      if (!diagnostic.empty()) {
         throw std::runtime_error(diagnostic);
      }
   }
   const bool cuePresent = mountPathExists(record.cuePath);
   const bool audioPresent = record.location == CueLocation::Staged && mountPathExists(record.audioPath);
   if (detached) {
      detached(record);
   }
   store.mark(record, MountState::Detached);
   store.removeDetached(record);
   std::string result = "Detached " + std::string(1, actual->letter) + ":; ";
   if (record.location == CueLocation::Existing) {
      result += "user CUE retained: " + pathToUtf8(record.cuePath);
   } else if (cuePresent) {
      result += "removed generated CUE: " + pathToUtf8(record.cuePath);
   } else {
      result += "generated CUE already absent";
   }
   if (audioPresent) {
      result += "\nRemoved staged " + std::string(record.storage == AudioStorage::Copy ? "compressed copy: " : "hard link: ") +
                pathToUtf8(record.audioPath);
   }
   return result + "\nMount state removed; original FLAC retained.\n";
}

std::string ImgDriveBackend::unmount(char requestedDrive) {
   const auto drive = normalizeDrive(requestedDrive);
   std::optional<MountRecord> selected;
   const auto currentDrives = runner.opticalDrives();
   for (const auto& record : store.records()) {
      const auto* current = findDevice(currentDrives, record.deviceIdentity);
      if (current && current->letter == drive) {
         if (selected) {
            throw std::runtime_error("ambiguous actual-device records; refusing to unmount");
         }
         selected = record;
      }
   }
   if (!selected) {
      throw std::runtime_error("no FlacCD-owned mount bound to this actual drive; inspect status (legacy/unverified records need manual recovery)");
   }
   auto record = *selected;
   store.validate(record);
   if (record.state == MountState::Preparing) {
      throw std::runtime_error("preparation never reached ImgDrive; resources preserved for ownership inspection");
   }
   auto drives = runner.opticalDrives();
   const auto* actual = findDevice(drives, record.deviceIdentity);
   if (!actual || !actual->imgDrive) {
      throw std::runtime_error("recorded optical device unavailable; no unmount issued; resources retained\n" + describeDrives(drives));
   }
   if (actual->media == MediaState::Unknown || actual->media == MediaState::Missing) {
      throw std::runtime_error("actual device state uncertain; resources retained");
   }
   const bool uncertainTarget = !record.deviceConfirmed;
   const auto beforeUnmount = drives;
   store.mark(record, MountState::Unmounting);
   record.state = MountState::Unmounting;
   if (uncertainTarget || actual->media != MediaState::Empty) {
      // Image-specific selector: never eject a drive merely because its letter
      // resembles a stale request, and never unmount all ImgDrive images.
      invoke({"-u", pathToUtf8(record.cuePath)});
   }
   const auto deadline = std::chrono::steady_clock::now() + timeout;
   do {
      drives = runner.opticalDrives();
      actual = findDevice(drives, record.deviceIdentity);
      bool empty = actual && actual->imgDrive && actual->media == MediaState::Empty;
      if (uncertainTarget) {
         // Without an observed TOC binding, the original selector may have
         // fallen back elsewhere. An empty requested drive alone proves nothing.
         // Image-specific unmount must succeed and every visible optical
         // device must be accounted for and empty; unrelated media is untouched.
         for (const auto& prior : beforeUnmount) {
            const auto* current = findDevice(drives, prior.identity);
            if (!current || current->media != MediaState::Empty) {
               empty = false;
            }
         }
         for (const auto& current : drives) {
            if (current.media != MediaState::Empty) {
               empty = false;
            }
         }
      }
      if (empty) {
         return cleanupEjected(record);
      }
      runner.pause(std::chrono::milliseconds(100));
   } while (std::chrono::steady_clock::now() < deadline);
   throw std::runtime_error("unmount not confirmed on actual device; CUE/state retained\n" + describeDrives(drives));
}

std::vector<MountStatus> ImgDriveBackend::status() {
   std::vector<MountStatus> result;
   for (const auto& record : store.records()) {
      const auto drives = runner.opticalDrives();
      const auto* actual = findDevice(drives, record.deviceIdentity);
      auto observed = record;
      if (actual) {
         observed.actualDrive = actual->letter;
      }
      result.push_back({observed, actual ? actual->media : MediaState::Unknown});
   }
   return result;
}

std::string ImgDriveBackend::drives() {
   return describeDrives(runner.opticalDrives());
}

}
