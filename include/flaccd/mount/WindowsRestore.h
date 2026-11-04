#pragma once

#include "flaccd/mount/WindowsRestoreStartup.h"
#include "flaccd/mount/AgentProtocol.h"

namespace flaccd {

AgentReply runWindowsRestore(const std::vector<std::string>& arguments);

}
