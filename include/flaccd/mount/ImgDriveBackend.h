#pragma once

#include "flaccd/mount/MountStateStore.h"
#include "flaccd/mount/ProcessRunner.h"

#include <functional>

namespace flaccd {

struct MountStatus {
   MountRecord record;
   MediaState media = MediaState::Unknown;
};

using MountPrepared = std::function<void(const MountRecord& record)>;
using MountDetached = std::function<void(const MountRecord& record)>;

class ImgDriveBackend {
public:
   ImgDriveBackend(ProcessRunner& runner,
                   const std::filesystem::path& executable,
                   const std::filesystem::path& cacheRoot,
                   std::chrono::milliseconds timeout = std::chrono::seconds(30),
                   MountFileSystem* fileSystem = nullptr,
                   MountLog log = {},
                   MountDetached detached = {});
   MountRecord mount(const std::filesystem::path& source,
                     std::optional<char> drive = std::nullopt,
                     std::string_view layoutSource = "auto",
                     const MountPrepared& prepared = {},
                     bool noCopy = false);
   std::string unmount(char drive);
   std::string cleanupEjected(const MountRecord& record, bool recovery = false);
   // Empty text means a positively identified, previously confirmed device is
   // empty and no other visible virtual/uncertain drive could use its resources.
   std::string recoveryDiagnostic(const MountRecord& record);
   std::string reconcileMissingMounts();
   std::vector<MountStatus> status();
   std::string drives();

private:
   MountRecord prepare(char drive,
                       const std::filesystem::path& source,
                       const FlacMetadata& metadata,
                       const TrackLayout& layout,
                       bool noCopy);
   ProcessResult invoke(std::vector<std::string> arguments);
   ProcessRunner& runner;
   std::filesystem::path executable;
   std::chrono::milliseconds timeout;
   MountFileSystem nativeFiles;
   MountFileSystem& files;
   MountLog log;
   MountDetached detached;
   std::unique_ptr<MountLock> guard;
   MountStateStore store;
};

}
