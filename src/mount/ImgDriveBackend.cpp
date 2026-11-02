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

void validateCachedSource(const MountRecord& record,
                          const FlacMetadata& metadata,
                          const TrackLayout& layout) {
   // Read the file actually handed to ImgDrive, not just the in-memory writer
   // output. Neither CUE creation nor this check creates a cached audio alias.
   std::ifstream input(record.cuePath, std::ios::binary);
   if (!input) {
      throw std::runtime_error("cannot open cached CUE: " + pathToUtf8(record.cuePath));
   }
   const std::string contents{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
   if (input.bad() || contents != record.cueText) {
      throw std::runtime_error("cached CUE differs from the prepared mount record");
   }
   const auto parsed = parseCueTextMetadata(contents);
   const auto reference = pathFromUtf8(parsed.fileName);
   if (parsed.fileType != "WAVE" || !reference.is_absolute() ||
       !compareTrackLayouts(layout, TrackLayout::fromText(metadata, parsed)).empty() ||
       !std::filesystem::is_regular_file(reference) || !std::filesystem::equivalent(reference, record.sourcePath)) {
      throw std::runtime_error("cached CUE must reference the original FLAC by its absolute path");
   }
   std::ifstream source(reference, std::ios::binary);
   char signature[4]{};
   source.read(signature, sizeof(signature));
   if (!source || std::string_view(signature, sizeof(signature)) != "fLaC") {
      throw std::runtime_error("cached CUE's original FLAC reference is missing, unreadable, or changed: " + parsed.fileName);
   }
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
                                 std::chrono::milliseconds operationTimeout)
   : runner(host), executable(validateExecutable(executablePath)), timeout(validateTimeout(operationTimeout)),
     guard(runner.lock(cacheRoot, timeout)), store(cacheRoot) {
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
                                   const MountPrepared& prepared) {
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
   const auto cue = generateFlacCueSheet(layout, metadata, original);
   const auto record = store.create(drive, original, cue);
   store.validate(record);
   // Verify again immediately before invoking the mounter. No implicit -m or
   // default-drive fallback can replace a different drive accidentally.
   if (runner.inspectDrive(drive) != MediaState::Empty) {
      throw std::runtime_error("drive changed during preparation; prepared CUE retained for safe recovery");
   }
   validateCachedSource(record, metadata, layout);
   if (prepared) {
      prepared(record);
      // Diagnostic callbacks may run application code; recheck before handing off.
      store.validate(record);
      validateCachedSource(record, metadata, layout);
      if (runner.inspectDrive(drive) != MediaState::Empty) {
         throw std::runtime_error("drive changed during preparation; prepared CUE retained for safe recovery");
      }
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
