#include "flaccd/mount/MountStateStore.h"
#include "flaccd/PathText.h"

#include <array>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <random>
#include <set>
#include <sstream>
#include <stdexcept>

namespace flaccd {
namespace {

constexpr std::string_view CACHE_MAGIC = "FlacCD ImgDrive cache v1\n";
constexpr std::string_view OLD_MAGIC = "FlacCD ImgDrive mount v1";
constexpr std::string_view STATE_MAGIC = "FlacCD ImgDrive mount v2";
constexpr std::array<MountState, 5> PHASES = {MountState::Prepared, MountState::Mounting,
                                            MountState::Mounted, MountState::Unmounting, MountState::Detached};

bool validId(std::string_view id) {
   if (id.size() != 32) {
      return false;
   }
   for (const auto character : id) {
      if (!((character >= '0' && character <= '9') || (character >= 'a' && character <= 'f'))) {
         return false;
      }
   }
   return true;
}

std::string makeId() {
   std::random_device generator;
   std::ostringstream text;
   for (int position = 0; position < 4; ++position) {
      text << std::hex << std::setfill('0') << std::setw(8) << static_cast<std::uint32_t>(generator());
   }
   return text.str();
}

void writeMetadata(const std::filesystem::path& path,
                    const std::string& text,
                    const std::filesystem::path& temporaryDirectory) {
   if (text.size() > 1024 * 1024) {
      throw std::runtime_error("mount metadata too large");
   }
   MountFileSystem files;
   // Publish only a complete, flushed immutable record. The caller holds the
   // cache lock; incomplete temporary writes never masquerade as phase markers.
   if (mountPathExists(path)) {
      throw std::runtime_error("mount metadata already exists; refusing replacement");
   }
   const auto temporary = temporaryDirectory / (".pending-" + makeId());
   const auto error = files.createCueExclusive(temporary, text);
   if (error) {
      throw std::system_error(error, "exclusive mount metadata creation: " + pathToUtf8(path));
   }
   publishMountMetadata(temporary, path);
}

bool regular(const std::filesystem::path& path) {
   return std::filesystem::is_regular_file(std::filesystem::symlink_status(path));
}

void removeMetadata(const std::filesystem::path& directory) {
   // Known files only. Never recurse and never follow a path from a manifest.
   for (const auto& entry : std::filesystem::directory_iterator(directory)) {
      const auto name = pathToUtf8(entry.path().filename());
      bool known = name == "mount.state" || name == "resources.state" || name == "device.state" || name == "actual.state";
      for (const auto phase : PHASES) {
         known = known || name == mountStateName(phase);
      }
      if (!known || !regular(entry.path())) {
         throw std::runtime_error("unrecognized cache contents; preserved for inspection");
      }
   }
   for (const auto phase : PHASES) {
      if (phase != MountState::Detached) {
         std::filesystem::remove(directory / mountStateName(phase));
      }
   }
   std::filesystem::remove(directory / "actual.state");
   std::filesystem::remove(directory / "device.state");
   std::filesystem::remove(directory / "resources.state");
   std::filesystem::remove(directory / "mount.state");
   std::filesystem::remove(directory / "detached");
   std::filesystem::remove(directory);
}

void retireMetadata(const std::filesystem::path& directory) {
   // Resources have already been safely removed. Retire the active record
   // atomically before deleting its metadata so a crash mid-deletion cannot
   // leave a malformed mount-* directory blocking every future request.
   const auto retired = directory.parent_path() / (".retired-" + pathToUtf8(directory.filename()));
   publishMountMetadata(directory, retired);
   removeMetadata(retired);
}

}

std::string mountStateName(MountState state) {
   switch (state) {
   case MountState::Preparing: return "preparing";
   case MountState::Prepared: return "prepared";
   case MountState::Mounting: return "mounting";
   case MountState::Mounted: return "mounted";
   case MountState::Unmounting: return "unmounting";
   case MountState::Detached: return "detached";
   }
   throw std::logic_error("invalid mount state");
}

std::string mountStrategyName(const MountRecord& record) {
   switch (record.location) {
   case CueLocation::Existing: return "existing user CUE";
   case CueLocation::Adjacent: return "generated adjacent CUE";
   case CueLocation::Legacy: return "legacy cache CUE";
   case CueLocation::Staged:
      return record.storage == AudioStorage::Copy ? "staged compressed FLAC copy" : "staged FLAC hard link";
   }
   throw std::logic_error("invalid mount strategy");
}

MountStateStore::MountStateStore(const std::filesystem::path& cacheRoot, MountFileSystem* fileSystem)
   : root(std::filesystem::absolute(cacheRoot).lexically_normal()), files(fileSystem ? *fileSystem : nativeFiles) {
   if (std::filesystem::is_symlink(std::filesystem::symlink_status(root))) {
      throw std::runtime_error("cache root must not be a symlink");
   }
   std::filesystem::create_directories(root);
   const auto marker = root / "cache.owner";
   if (!mountPathExists(marker)) {
      if (!std::filesystem::is_empty(root)) {
         throw std::runtime_error("refusing nonempty cache without a FlacCD ownership marker");
      }
      writeMetadata(marker, std::string(CACHE_MAGIC), root);
   }
   if (readMountCue(marker) != CACHE_MAGIC) {
      throw std::runtime_error("invalid FlacCD cache ownership marker");
   }
}

MountRecord MountStateStore::load(const std::filesystem::path& directory) const {
   const auto name = pathToUtf8(directory.filename());
   if (!name.starts_with("mount-") || !validId(name.substr(6)) ||
       std::filesystem::is_symlink(std::filesystem::symlink_status(directory))) {
      throw std::runtime_error("unsafe mount directory");
   }
   std::istringstream input(readMountCue(directory / "mount.state"));
   std::string magic;
   std::getline(input, magic);
   MountRecord record;
   std::string source;
   int location = 0;
   input >> record.id >> record.drive >> std::quoted(source) >> std::quoted(record.cueText);
   if (!input || (magic != STATE_MAGIC && magic != OLD_MAGIC) || record.id != name.substr(6) ||
       record.drive < 'D' || record.drive > 'Z') {
      throw std::runtime_error("malformed mount state; preserve cache for inspection");
   }
   if (magic == STATE_MAGIC) {
      input >> location;
      if (!input || location < 0 || location > 3) {
         throw std::runtime_error("invalid mount location");
      }
   }
   input >> std::ws;
   if (!input.eof()) {
      throw std::runtime_error("unexpected trailing mount state data");
   }
   record.location = static_cast<CueLocation>(location);
   record.sourcePath = pathFromUtf8(source);
   record.entryPath = directory;
   if (!record.sourcePath.is_absolute()) {
      throw std::runtime_error("mount source must be absolute");
   }
   auto adjacent = record.sourcePath;
   adjacent.replace_extension(".cue");
   record.cuePath = record.location == CueLocation::Legacy ? directory / "disc.cue" :
                    record.location == CueLocation::Staged ? directory / adjacent.filename() : adjacent;
   record.audioPath = record.location == CueLocation::Staged ? directory / record.sourcePath.filename() : record.sourcePath;
   if (record.location != CueLocation::Existing &&
       !record.cueText.starts_with("REM FlacCD ImgDrive mount " + record.id + "\n")) {
      throw std::runtime_error("invalid mount ownership data");
   }
   const auto resources = directory / "resources.state";
   if (mountPathExists(resources)) {
      std::istringstream resourceInput(readMountCue(resources));
      std::string id;
      int storage = 0;
      resourceInput >> id >> storage >> std::quoted(record.cueIdentity) >> std::quoted(record.audioIdentity);
      resourceInput >> std::ws;
      if (!resourceInput || !resourceInput.eof() || id != record.id || storage < 0 || storage > 2 ||
          record.cueIdentity.empty() || record.audioIdentity.empty()) {
         throw std::runtime_error("invalid resource identity record");
      }
      record.storage = static_cast<AudioStorage>(storage);
      record.resourcesReady = true;
      record.state = MountState::Prepared;
   } else if (magic == OLD_MAGIC) {
      record.resourcesReady = true;
      record.state = MountState::Prepared;
   }
   for (const auto phase : PHASES) {
      const auto marker = directory / mountStateName(phase);
      if (mountPathExists(marker)) {
         if (readMountCue(marker) != record.id || !record.resourcesReady) {
            throw std::runtime_error("partial or invalid mount phase marker; preserve resources");
         }
         record.state = phase;
      }
   }
   for (const auto bindingName : {"device.state", "actual.state"}) {
      if (mountPathExists(directory / bindingName)) {
         record.deviceConfirmed = std::string_view(bindingName) == "actual.state";
         std::istringstream binding(readMountCue(directory / bindingName));
         std::string id;
         binding >> id >> record.actualDrive >> std::quoted(record.deviceIdentity) >> std::quoted(record.imgDriveSelector);
         if (!binding || id != record.id || record.actualDrive < 'D' || record.actualDrive > 'Z' ||
             record.deviceIdentity.empty() || record.imgDriveSelector != std::string(1, record.actualDrive)) {
            throw std::runtime_error("invalid optical device binding; resources preserved");
         }
         binding >> std::ws;
         if (!binding.eof()) {
            throw std::runtime_error("unexpected device binding data");
         }
      }
   }
   return record;
}

std::vector<MountRecord> MountStateStore::records() const {
   std::vector<MountRecord> result;
   for (const auto& entry : std::filesystem::directory_iterator(root)) {
      if (pathToUtf8(entry.path().filename()).starts_with("mount-")) {
         result.push_back(load(entry.path()));
      }
   }
   return result;
}

MountRecord MountStateStore::begin(char drive,
                                   const std::filesystem::path& source,
                                   const std::string& cue,
                                   CueLocation location) {
   if (drive < 'D' || drive > 'Z' || !source.is_absolute()) {
      throw std::invalid_argument("invalid mount drive/source");
   }
   const auto id = makeId();
   const auto prepared = root / (".prepare-" + id);
   const auto directory = root / ("mount-" + id);
   if (!std::filesystem::create_directory(prepared)) {
      throw std::runtime_error("mount ID collision; retry");
   }
   const auto text = location == CueLocation::Existing ? cue : "REM FlacCD ImgDrive mount " + id + "\n" + cue;
   std::ostringstream manifest;
   manifest << STATE_MAGIC << '\n' << id << ' ' << drive << '\n' << std::quoted(pathToUtf8(source)) << '\n'
            << std::quoted(text) << '\n' << static_cast<int>(location) << '\n';
   writeMetadata(prepared / "mount.state", manifest.str(), root);
   publishMountMetadata(prepared, directory);
   return load(directory);
}

MountRecord MountStateStore::complete(const MountRecord& record, AudioStorage storage) {
   const auto actual = load(root / ("mount-" + record.id));
   if (actual.resourcesReady || readMountCue(actual.cuePath) != actual.cueText) {
      throw std::runtime_error("prepared CUE changed before recording resource identities");
   }
   const auto cueIdentity = files.identity(actual.cuePath);
   const auto audioIdentity = files.identity(actual.audioPath);
   std::ostringstream text;
   text << record.id << ' ' << static_cast<int>(storage) << ' ' << std::quoted(cueIdentity) << ' '
        << std::quoted(audioIdentity) << '\n';
   writeMetadata(actual.entryPath / "resources.state", text.str(), root);
   auto result = load(actual.entryPath);
   mark(result, MountState::Prepared);
   return load(actual.entryPath);
}

MountRecord MountStateStore::create(char drive, const std::filesystem::path& source, const std::string& cue) {
   const auto record = begin(drive, source, cue, CueLocation::Legacy);
   const auto error = files.createCueExclusive(record.cuePath, record.cueText);
   if (error) {
      throw std::system_error(error, "create legacy cached CUE");
   }
   return complete(record, AudioStorage::Original);
}

void MountStateStore::validate(const MountRecord& record) const {
   if (!validId(record.id)) {
      throw std::runtime_error("invalid mount ID");
   }
   const auto actual = load(root / ("mount-" + record.id));
   if (actual.entryPath != record.entryPath || actual.audioPath != record.audioPath ||
       actual.cuePath != record.cuePath || actual.sourcePath != record.sourcePath || actual.drive != record.drive ||
       actual.cueText != record.cueText || actual.location != record.location) {
      throw std::runtime_error("mount record does not match owned cache");
   }
}

void MountStateStore::mark(const MountRecord& record, MountState state) {
   validate(record);
   const auto actual = load(record.entryPath);
   if (!actual.resourcesReady) {
      throw std::runtime_error("incomplete preparation; resources preserved for inspection");
   }
   const auto marker = actual.entryPath / mountStateName(state);
   if (mountPathExists(marker)) {
      if (readMountCue(marker) != record.id) {
         throw std::runtime_error("invalid phase marker");
      }
   } else {
      writeMetadata(marker, record.id, root);
   }
}

MountRecord MountStateStore::bindDevice(const MountRecord& record,
                                        char actualDrive,
                                        const std::string& identity,
                                        const std::string& selector) {
   validate(record);
   if (actualDrive < 'D' || actualDrive > 'Z' || identity.empty() || selector != std::string(1, actualDrive)) {
      throw std::invalid_argument("invalid device binding");
   }
   const auto actual = load(record.entryPath);
   const auto name = actual.deviceIdentity.empty() ? "device.state" : "actual.state";
   std::ostringstream text;
   text << record.id << ' ' << actualDrive << ' ' << std::quoted(identity) << ' ' << std::quoted(selector) << '\n';
   writeMetadata(actual.entryPath / name, text.str(), root);
   return load(actual.entryPath);
}

void MountStateStore::discardEmptyPreparation(const MountRecord& record) {
   const auto actual = load(root / ("mount-" + record.id));
   if (actual.resourcesReady || actual.state != MountState::Preparing) {
      throw std::runtime_error("cannot discard active mount resources");
   }
   retireMetadata(actual.entryPath);
}

void MountStateStore::removeDetached(const MountRecord& record) {
   validate(record);
   const auto actual = load(record.entryPath);
   if (actual.state != MountState::Detached) {
      throw std::runtime_error("cannot clean a mount without confirmed detachment");
   }
   std::set<std::string> allowed{"mount.state", "resources.state", "device.state", "actual.state"};
   for (const auto phase : PHASES) {
      allowed.insert(mountStateName(phase));
   }
   if (actual.cuePath.parent_path() == actual.entryPath) {
      allowed.insert(pathToUtf8(actual.cuePath.filename()));
   }
   if (actual.location == CueLocation::Staged) {
      allowed.insert(pathToUtf8(actual.audioPath.filename()));
   }
   for (const auto& entry : std::filesystem::directory_iterator(actual.entryPath)) {
      if (!allowed.contains(pathToUtf8(entry.path().filename())) || !regular(entry.path())) {
         throw std::runtime_error("unrecognized cache contents; nothing deleted");
      }
   }
   // Preflight every owned resource before removing any of them. Changed user
   // files are preserved even after the device has been successfully detached.
   if (actual.location != CueLocation::Existing && mountPathExists(actual.cuePath) &&
       ((!actual.cueIdentity.empty() && files.identity(actual.cuePath) != actual.cueIdentity) ||
        readMountCue(actual.cuePath) != actual.cueText)) {
      throw std::runtime_error("owned CUE was replaced or modified; preserved: " + pathToUtf8(actual.cuePath));
   }
   if (actual.location == CueLocation::Staged && mountPathExists(actual.audioPath) &&
       files.identity(actual.audioPath) != actual.audioIdentity) {
      throw std::runtime_error("staged audio changed; resources preserved");
   }
   if (actual.location == CueLocation::Adjacent) {
      for (const auto& other : records()) {
         if (other.id != actual.id && other.cuePath == actual.cuePath && other.state != MountState::Detached) {
            throw std::runtime_error("adjacent CUE is still referenced by another mount; preserved");
         }
      }
   }
   if (actual.location != CueLocation::Existing && mountPathExists(actual.cuePath)) {
      if (actual.cueIdentity.empty()) {
         // Old manifests only owned a fixed cache-local CUE; never an adjacent file.
         if (actual.location != CueLocation::Legacy || readMountCue(actual.cuePath) != actual.cueText) {
            throw std::runtime_error("legacy CUE ownership cannot be verified");
         }
         std::filesystem::remove(actual.cuePath);
      } else {
         files.removeOwned(actual.cuePath, actual.cueIdentity, &actual.cueText);
      }
   }
   if (actual.location == CueLocation::Staged && mountPathExists(actual.audioPath)) {
      files.removeOwned(actual.audioPath, actual.audioIdentity);
   }
   retireMetadata(actual.entryPath);
}

}
