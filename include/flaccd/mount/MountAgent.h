#pragma once

#include "flaccd/mount/AgentProtocol.h"
#include "flaccd/mount/ImgDriveBackend.h"

#include <atomic>
#include <map>
#include <mutex>

namespace flaccd {

// One serialized coordinator is shared by IPC clients and device notifications.
// No Windows dependencies: the real named pipe/window adapter lives separately.
class MountAgent {
public:
   MountAgent(ProcessRunner& host, std::filesystem::path executable, std::filesystem::path cache);
   AgentReply handle(const std::vector<std::string>& arguments);
   bool isStopping() const noexcept;
   std::string reconcile(std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());
private:
   ProcessRunner& host;
   std::filesystem::path executable;
   std::filesystem::path cache;
   std::mutex mutex;
   std::atomic<bool> stopping{false};
   std::map<std::string, std::chrono::steady_clock::time_point> emptySince;
};

}
