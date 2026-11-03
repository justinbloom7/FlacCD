#pragma once

#include "flaccd/mount/AgentProtocol.h"
#include "flaccd/mount/ImgDriveBackend.h"
#include "flaccd/mount/RestoreStore.h"

namespace flaccd {

// Budget both vendor launch and TOC verification without starting a mount when
// the helper has too little time left to observe a meaningful result.
std::optional<std::chrono::seconds> restoreAttemptTimeout(std::chrono::milliseconds remaining);

// One bounded unit of restoration. Exit 3 means retryable readiness or confirmed
// progress. Exit 1 is terminal/possibly uncertain and must never be replayed.
AgentReply restoreStep(ProcessRunner& host, ImgDriveBackend& backend, RestoreStore& store);
void captureDesiredMounts(ProcessRunner& host,
                           const std::filesystem::path& cache,
                           RestoreStore& store,
                           bool rearm = true);

}
