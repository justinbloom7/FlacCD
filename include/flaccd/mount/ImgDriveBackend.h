#pragma once

#include "flaccd/mount/MountStateStore.h"
#include "flaccd/mount/ProcessRunner.h"

namespace flaccd {

struct MountStatus {
   MountRecord record;
   MediaState media = MediaState::Unknown;
};

class ImgDriveBackend {
public:
   ImgDriveBackend(ProcessRunner& runner,
                   const std::filesystem::path& executable,
                   const std::filesystem::path& cacheRoot,
                   std::chrono::milliseconds timeout = std::chrono::seconds(30));
   MountRecord mount(const std::filesystem::path& source,
                     std::optional<char> drive = std::nullopt,
                     std::string_view layoutSource = "auto");
   void unmount(char drive);
   std::vector<MountStatus> status();
   std::string drives();

private:
   ProcessResult invoke(std::vector<std::string> arguments);
   ProcessRunner& runner;
   std::filesystem::path executable;
   std::chrono::milliseconds timeout;
   std::unique_ptr<MountLock> guard;
   MountStateStore store;
};

}
