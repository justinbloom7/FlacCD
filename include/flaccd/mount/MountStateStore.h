#pragma once

#include "flaccd/mount/MountFileSystem.h"

#include <vector>

namespace flaccd {

enum class MountState { Preparing, Prepared, Mounting, Mounted, Unmounting, Detached };
enum class CueLocation { Legacy, Existing, Adjacent, Staged };
enum class AudioStorage { Original, HardLink, Copy };

struct MountRecord {
   std::string id;
   char drive = 0;
   std::filesystem::path entryPath;
   std::filesystem::path sourcePath;
   std::filesystem::path cuePath;
   std::filesystem::path audioPath;
   std::string cueText;
   std::string cueIdentity;
   std::string audioIdentity;
   CueLocation location = CueLocation::Legacy;
   AudioStorage storage = AudioStorage::Original;
   MountState state = MountState::Preparing;
   bool resourcesReady = false;
};

std::string mountStateName(MountState state);
std::string mountStrategyName(const MountRecord& record);

// The per-cache interprocess lock is held throughout preparation/mount/unmount.
// An immutable intent is persisted BEFORE creating any adjacent or staged files.
class MountStateStore {
public:
   explicit MountStateStore(const std::filesystem::path& cacheRoot, MountFileSystem* fileSystem = nullptr);
   std::vector<MountRecord> records() const;
   MountRecord begin(char drive,
                     const std::filesystem::path& source,
                     const std::string& cue,
                     CueLocation location);
   MountRecord complete(const MountRecord& record, AudioStorage storage);
   void discardEmptyPreparation(const MountRecord& record);
   // Compatibility API for original cache-only mounts and their existing tests.
   MountRecord create(char drive, const std::filesystem::path& source, const std::string& cue);
   void mark(const MountRecord& record, MountState state);
   void removeDetached(const MountRecord& record);
   void validate(const MountRecord& record) const;

private:
   MountRecord load(const std::filesystem::path& directory) const;
   std::filesystem::path root;
   MountFileSystem nativeFiles;
   MountFileSystem& files;
};

}
