#include "flaccd/mount/RestoreCoordinator.h"
#include "flaccd/mount/MountCueDiscovery.h"
#include "flaccd/PathText.h"

#include <algorithm>

namespace flaccd {

void captureDesiredMounts(ProcessRunner& host,
                           const std::filesystem::path& cache,
                           RestoreStore& store,
                           bool rearm) {
   const auto session = host.sessionIdentity();
   if (session.empty()) {
      throw std::runtime_error("login session identity unavailable; restoration cannot be enabled safely");
   }
   auto state = store.read();
   for (auto& entry : state.mounts) {
      if (rearm && entry.suspended) {
         entry.suspended = false;
         entry.session = "explicit-rearm"; // Empty-media cleanup must not undo the user's fresh intent.
      }
   }
   store.save(state);
   const auto drives = host.opticalDrives();
   for (const auto& record : MountStateStore(cache).records()) {
      const auto* actual = findDevice(drives, record.deviceIdentity);
      if (!record.deviceConfirmed || record.state != MountState::Mounted || !actual ||
          !actual->imgDrive || actual->media != MediaState::Ready) {
         continue;
      }
      const auto metadata = FlacMetadataInspector{}.inspect(record.sourcePath);
      const auto saved = TrackLayout::fromText(metadata, parseCueTextMetadata(record.cueText));
      for (const auto selection : {"auto", "native", "text"}) {
         try {
            const auto candidate = discoverMountLayout(metadata, record.sourcePath, selection);
            if (compareTrackLayouts(saved, candidate).empty() && host.matchesLayout(actual->letter, candidate)) {
               store.remember(record, selection, record.storage != AudioStorage::Copy, session);
               break;
            }
         } catch (const std::invalid_argument&) {
            // Try the explicit layout alternatives for old overridden mounts.
         }
      }
   }
}

AgentReply restoreStep(ProcessRunner& host, ImgDriveBackend& backend, RestoreStore& store) {
   AgentReply reply;
   if (!store.read().enabled) {
      reply.output = "Automatic restoration is disabled.\n";
      return reply;
   }
   const auto session = host.sessionIdentity();
   if (session.empty()) {
      throw std::runtime_error("login session identity unavailable; restoration refused");
   }
   reply.diagnostics = backend.reconcileMissingMounts();
   const auto state = store.read(); // Detachment may have removed same-session eject intent.
   const auto records = backend.status();
   const auto drives = host.opticalDrives();
   bool pending = false;
   bool suspended = false;
   for (const auto& entry : state.mounts) {
      const auto sourceText = pathToUtf8(entry.source);
      if (entry.suspended) {
         reply.diagnostics += "Suspended restore entry (explicit enable can rearm): " + sourceText + "\n";
         suspended = true;
         continue;
      }
      const auto active = std::find_if(records.begin(), records.end(), [&](const auto& status) {
         return status.record.sourcePath == entry.source;
      });
      if (active != records.end()) {
         if (active->record.id == entry.mountId && active->record.deviceConfirmed && active->media == MediaState::Ready) {
            // Never mount a duplicate. Rebind intent to this login only after
            // checking the recorded layout against the actual media's TOC.
            const auto metadata = FlacMetadataInspector{}.inspect(active->record.audioPath);
            const auto layout = TrackLayout::fromText(metadata, parseCueTextMetadata(active->record.cueText));
            if (host.matchesLayout(active->record.actualDrive, layout)) {
               if (entry.session != session) {
                  store.remember(active->record, entry.layout, entry.noCopy, session);
               }
               continue;
            }
         }
         reply.diagnostics += "Existing/uncertain mount prevents duplicate restoration: " + sourceText + "\n";
         pending = true;
         continue;
      }
      std::error_code error;
      if (!std::filesystem::is_regular_file(entry.source, error)) {
         reply.diagnostics += "Restore source missing/unreadable; desired entry retained: " + sourceText + "\n";
         pending = true;
         continue;
      }
      const auto* target = findDevice(drives, entry.deviceIdentity);
      bool safe = target && target->imgDrive && target->media == MediaState::Empty;
      for (const auto& drive : drives) {
         safe = safe && !drive.identity.empty() && drive.media != MediaState::Unknown &&
                drive.media != MediaState::Missing;
         // An untracked ready virtual image could already be this FLAC.
         if (drive.imgDrive && drive.media == MediaState::Ready) {
            const auto tracked = std::find_if(records.begin(), records.end(), [&](const auto& status) {
               return status.record.deviceIdentity == drive.identity && status.record.deviceConfirmed &&
                      status.record.sourcePath != entry.source;
            });
            safe = safe && tracked != records.end();
         }
      }
      if (!safe) {
         reply.diagnostics += "ImgDrive device not safely ready; no image replaced: " + sourceText + "\n";
         pending = true;
         continue;
      }
      // Persist an uncertainty fence BEFORE invoking the vendor. A lost reply,
      // crash or timeout must not cause automatic replay next login.
      auto fenced = store.read();
      for (auto& current : fenced.mounts) {
         if (current.source == entry.source) {
            current.suspended = true;
         }
      }
      store.save(fenced);
      try {
         const auto mounted = backend.mount(entry.source, target->letter, entry.layout, {}, entry.noCopy);
         store.remember(mounted, entry.layout, entry.noCopy, session);
         reply.exitCode = 3; // Continue with the next entry in a fresh snapshot.
         reply.output = "Restored " + sourceText + " on " + std::string(1, mounted.actualDrive) + ":\n";
      } catch (const std::exception& problem) {
         reply.exitCode = 1;
         reply.diagnostics += "Restoration failed/uncertain; entry suspended, resources preserved: " +
                              std::string(problem.what()) + "\n";
      }
      return reply;
   }
   reply.exitCode = pending ? 3 : suspended ? 1 : 0;
   if (reply.exitCode == 0) {
      reply.output = "Restoration complete; no duplicate mounts requested.\n";
   }
   return reply;
}

}
