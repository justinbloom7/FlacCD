#pragma once

#include "flaccd/mount/MountStateStore.h"

namespace flaccd {

struct DesiredMount {
   std::filesystem::path source;
   std::string layout = "auto";
   char drive = 0;
   std::string deviceIdentity;
   bool noCopy = false;
   std::string mountId;
   std::string session;
   bool suspended = false;
};

struct RestoreState {
   bool enabled = false;
   std::string startupCommand;
   std::string previousStartupCommand; // Exact prior ownership during a crash-interrupted registry update.
   std::vector<DesiredMount> mounts;
};

// A separate, versioned desired list. Callers hold the per-cache lock.
// Snapshots are immutable and atomically published; a malformed newest snapshot
// fails closed rather than resurrecting intent from an older snapshot.
class RestoreStore {
public:
   explicit RestoreStore(std::filesystem::path cache);
   RestoreState read() const;
   // Distinguishes a saved disabled preference from an unconfigured install.
   // Call read() first so malformed published state still fails closed.
   bool hasConfiguration() const;
   void save(const RestoreState& state);
   void remember(const MountRecord& record, std::string_view layout, bool noCopy, const std::string& session);
   void forget(const MountRecord& record);
   void observedDetach(const MountRecord& record, const std::string& session);
   void suspend();

private:
   std::filesystem::path root;
};

}
