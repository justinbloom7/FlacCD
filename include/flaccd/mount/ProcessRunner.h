#pragma once

#include "flaccd/TrackLayout.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace flaccd {

enum class MediaState { Missing, Empty, Ready, Unknown };

struct ProcessResult {
   std::uint32_t exitCode = 0;
   bool timedOut = false;
   std::string diagnostics;
};

class MountLock {
public:
   virtual ~MountLock() = default;
};

class ProcessRunner {
public:
   virtual ~ProcessRunner() = default;
   virtual ProcessResult run(const std::filesystem::path& executable,
                             std::span<const std::string> arguments,
                             std::chrono::milliseconds timeout) = 0;
   virtual MediaState inspectDrive(char drive) = 0;
   virtual bool matchesLayout(char drive, const TrackLayout& layout) = 0;
   // nullopt means no verified, machine-readable enumeration is available.
   virtual std::optional<std::vector<char>> emptyDrives() = 0;
   virtual std::unique_ptr<MountLock> lock(const std::filesystem::path& cache,
                                           std::chrono::milliseconds timeout) = 0;
   virtual void pause(std::chrono::milliseconds duration) = 0;
};

// Standard Windows CRT argv quoting, independent of a shell. Kept portable
// so spaces, quotes, backslashes, and Unicode can be unit tested on Linux.
std::wstring quoteWindowsArgument(std::wstring_view argument);

}
