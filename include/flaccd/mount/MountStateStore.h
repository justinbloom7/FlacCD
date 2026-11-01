#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace flaccd {

enum class MountState { Prepared, Mounting, Mounted, Unmounting, Detached };

struct MountRecord {
   std::string id;
   char drive = 0;
   std::filesystem::path sourcePath;
   std::filesystem::path cuePath;
   std::string cueText;
   MountState state = MountState::Prepared;
};

std::string mountStateName(MountState state);

// Caller holds the per-cache interprocess lock throughout each operation.
// Each mount has an immutable manifest/CUE and append-only phase markers.
// No paths from a manifest are used as deletion targets.
class MountStateStore {
public:
   explicit MountStateStore(const std::filesystem::path& cacheRoot);
   std::vector<MountRecord> records() const;
   MountRecord create(char drive, const std::filesystem::path& source, const std::string& cue);
   void mark(const MountRecord& record, MountState state);
   void removeDetached(const MountRecord& record);
   void validate(const MountRecord& record) const;

private:
   MountRecord load(const std::filesystem::path& directory) const;
   std::filesystem::path root;
};

}
