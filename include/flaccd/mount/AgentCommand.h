#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <vector>

namespace flaccd {

// Keep the original wire command/alias for compatibility with existing agents.
std::vector<std::string> normalizeAgentCommand(std::vector<std::string> arguments);
// Validate stop arguments before connecting; no installation/cache is required.
std::optional<std::chrono::milliseconds> agentStopTimeout(const std::vector<std::string>& arguments);

}
