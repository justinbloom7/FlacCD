#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace flaccd {

constexpr std::size_t MAX_AGENT_MESSAGE = 1024 * 1024;

struct AgentReply {
   std::uint32_t exitCode = 0;
   std::string output;
   std::string diagnostics;
   // Version 1 status rows: ID, requested letter, actual letter, PnP identity,
   // phase, strategy, original FLAC, CUE, audio resource, media state, ImgDrive CLI selector.
   std::vector<std::vector<std::string>> records;
};

std::string encodeAgentRequest(const std::vector<std::string>& arguments);
std::vector<std::string> decodeAgentRequest(std::string_view message);
std::string encodeAgentReply(const AgentReply& reply);
AgentReply decodeAgentReply(std::string_view message);

}
