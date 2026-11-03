#pragma once

#include "flaccd/mount/WindowsAgentIpc.h"

namespace flaccd {

// Defaults discover the installed backend and sibling agent. Injectable settings
// let native CLI tests use an isolated endpoint and harmless agent helper.
struct AgentClientSettings {
   std::filesystem::path imgDrive;
   std::filesystem::path cache;
   std::filesystem::path agentExecutable;
   std::wstring endpointTag;
};

AgentReply runWindowsAgentClient(std::vector<std::string> arguments, AgentClientSettings settings = {});

}
