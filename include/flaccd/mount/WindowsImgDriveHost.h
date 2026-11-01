#pragma once

#include "flaccd/mount/ProcessRunner.h"

namespace flaccd {

// Built only on Windows. Discovery uses explicit path, FLACCD_IMGDRIVE,
// registered App Paths, then PATH; no guessed Program Files location.
std::filesystem::path findImgDrive(const std::filesystem::path& explicitPath = {});
std::filesystem::path defaultMountCache();

class WindowsImgDriveHost : public ProcessRunner {
public:
   ProcessResult run(const std::filesystem::path& executable,
                     std::span<const std::string> arguments,
                     std::chrono::milliseconds timeout) override;
   MediaState inspectDrive(char drive) override;
   bool matchesLayout(char drive, const TrackLayout& layout) override;
   std::optional<std::vector<char>> emptyDrives() override;
   std::unique_ptr<MountLock> lock(const std::filesystem::path& cache,
                                   std::chrono::milliseconds timeout) override;
   void pause(std::chrono::milliseconds duration) override;
};

}
