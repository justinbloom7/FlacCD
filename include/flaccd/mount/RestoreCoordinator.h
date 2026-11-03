#pragma once

#include "flaccd/mount/AgentProtocol.h"
#include "flaccd/mount/ImgDriveBackend.h"
#include "flaccd/mount/RestoreStore.h"

namespace flaccd {

// One bounded unit of restoration. Exit 3 means retryable readiness or confirmed
// progress. Exit 1 is terminal/possibly uncertain and must never be replayed.
AgentReply restoreStep(ProcessRunner& host, ImgDriveBackend& backend, RestoreStore& store);
void captureDesiredMounts(ProcessRunner& host,
                           const std::filesystem::path& cache,
                           RestoreStore& store,
                           bool rearm = true);

}
