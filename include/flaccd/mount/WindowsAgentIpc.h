#pragma once

#include "flaccd/mount/AgentProtocol.h"

#include <filesystem>
#include <chrono>

namespace flaccd {

// Windows-only transport. endpointTag is reserved for isolated test helpers;
// the production CLI/agent always use the single default per-user endpoint.
AgentReply requestWindowsAgent(const std::vector<std::string>& arguments,
                               const std::filesystem::path& imgDrive,
                               const std::filesystem::path& cache,
                               const std::filesystem::path& agentExecutable,
                               const std::wstring& endpointTag = {},
                               std::chrono::milliseconds requestTimeout = std::chrono::milliseconds::zero());
int runWindowsAgent(const std::filesystem::path& imgDrive,
                    const std::filesystem::path& cache,
                    const std::wstring& endpointTag = {});

}
