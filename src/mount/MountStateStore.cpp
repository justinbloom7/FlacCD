#include "flaccd/mount/MountStateStore.h"
#include "flaccd/PathText.h"

#include <array>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <random>
#include <set>
#include <sstream>
#include <stdexcept>

namespace flaccd {
namespace {

constexpr std::string_view CACHE_MAGIC = "FlacCD ImgDrive cache v1\n";
constexpr std::string_view STATE_MAGIC = "FlacCD ImgDrive mount v1";
constexpr std::array<MountState, 5> PHASES = {MountState::Prepared, MountState::Mounting,
                                            MountState::Mounted, MountState::Unmounting, MountState::Detached};
constexpr std::uintmax_t MAX_STATE_BYTES = 1024 * 1024;

bool isRegular(const std::filesystem::path& path) {
   const auto status = std::filesystem::symlink_status(path);
   return std::filesystem::is_regular_file(status) && !std::filesystem::is_symlink(status);
}

std::string readFile(const std::filesystem::path& path) {
   if (!isRegular(path) || std::filesystem::file_size(path) > MAX_STATE_BYTES) {
      throw std::runtime_error("unsafe or oversized cache file: " + pathToUtf8(path));
   }
   std::ifstream input(path, std::ios::binary);
   if (!input) {
      throw std::runtime_error("cannot read cache file: " + pathToUtf8(path));
   }
   std::string text{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
   if (input.bad()) {
      throw std::runtime_error("failed reading cache file");
   }
   return text;
}

void writeFile(const std::filesystem::path& path, const std::string& text) {
   if (std::filesystem::exists(std::filesystem::symlink_status(path))) {
      throw std::runtime_error("refusing existing cache file");
   }
   std::ofstream output(path, std::ios::binary);
   output.write(text.data(), static_cast<std::streamsize>(text.size()));
   output.close();
   if (!output) {
      throw std::runtime_error("failed writing cache file: " + pathToUtf8(path));
   }
}

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

}

std::string mountStateName(MountState state) {
   switch (state) {
   case MountState::Prepared: return "prepared";
   case MountState::Mounting: return "mounting";
   case MountState::Mounted: return "mounted";
   case MountState::Unmounting: return "unmounting";
   case MountState::Detached: return "detached";
   }
   throw std::logic_error("invalid mount state");
}

MountStateStore::MountStateStore(const std::filesystem::path& cacheRoot)
   : root(std::filesystem::absolute(cacheRoot).lexically_normal()) {
   if (std::filesystem::is_symlink(std::filesystem::symlink_status(root))) {
      throw std::runtime_error("cache root must not be a symlink");
   }
   std::filesystem::create_directories(root);
   const auto marker = root / "cache.owner";
   if (!std::filesystem::exists(std::filesystem::symlink_status(marker))) {
      if (!std::filesystem::is_empty(root)) {
         throw std::runtime_error("refusing nonempty cache without a FlacCD ownership marker");
      }
      writeFile(marker, std::string(CACHE_MAGIC));
   }
   if (readFile(marker) != CACHE_MAGIC) {
      throw std::runtime_error("invalid FlacCD cache ownership marker");
   }
}

MountRecord MountStateStore::load(const std::filesystem::path& directory) const {
   const auto name = directory.filename().string();
   if (!name.starts_with("mount-") || !validId(name.substr(6)) ||
       std::filesystem::is_symlink(std::filesystem::symlink_status(directory))) {
      throw std::runtime_error("unsafe mount directory");
   }
   const auto text = readFile(directory / "mount.state");
   std::istringstream input(text);
   std::string magic;
   std::getline(input, magic);
   MountRecord record;
   std::string source;
   input >> record.id >> record.drive >> std::quoted(source) >> std::quoted(record.cueText);
   if (!input || magic != STATE_MAGIC || record.id != name.substr(6) ||
       record.drive < 'D' || record.drive > 'Z') {
      throw std::runtime_error("malformed mount state; preserve cache for manual inspection");
   }
   input >> std::ws;
   if (!input.eof()) {
      throw std::runtime_error("unexpected trailing mount state data");
   }
   record.sourcePath = pathFromUtf8(source);
   record.cuePath = directory / "disc.cue";
   const auto prefix = "REM FlacCD ImgDrive mount " + record.id + "\n";
   if (!record.sourcePath.is_absolute() || !record.cueText.starts_with(prefix)) {
      throw std::runtime_error("mount manifest has invalid source/ownership data");
   }
   for (const auto phase : PHASES) {
      const auto marker = directory / mountStateName(phase);
      if (std::filesystem::exists(std::filesystem::symlink_status(marker))) {
         if (readFile(marker) != record.id) {
            throw std::runtime_error("invalid mount phase marker");
         }
         record.state = phase;
      }
   }
   if (record.state != MountState::Detached || std::filesystem::exists(std::filesystem::symlink_status(record.cuePath))) {
      if (readFile(record.cuePath) != record.cueText) {
         throw std::runtime_error("cached CUE changed; refusing mount operations or deletion");
      }
   }
   return record;
}

std::vector<MountRecord> MountStateStore::records() const {
   std::vector<MountRecord> result;
   for (const auto& entry : std::filesystem::directory_iterator(root)) {
      if (entry.path().filename().string().starts_with("mount-")) {
         result.push_back(load(entry.path()));
      }
   }
   return result;
}

MountRecord MountStateStore::create(char drive, const std::filesystem::path& source, const std::string& cue) {
   if (drive < 'D' || drive > 'Z' || !source.is_absolute()) {
      throw std::invalid_argument("invalid mount drive/source");
   }
   const auto id = makeId();
   const auto prepared = root / (".prepare-" + id);
   const auto finalDirectory = root / ("mount-" + id);
   if (!std::filesystem::create_directory(prepared)) {
      throw std::runtime_error("mount cache name collision; retry the command");
   }
   const auto ownedCue = "REM FlacCD ImgDrive mount " + id + "\n" + cue;
   std::ostringstream manifest;
   manifest << STATE_MAGIC << '\n' << id << ' ' << drive << '\n'
            << std::quoted(pathToUtf8(source)) << '\n' << std::quoted(ownedCue) << '\n';
   if (manifest.str().size() > MAX_STATE_BYTES) {
      throw std::invalid_argument("mount manifest too large");
   }
   // Interrupted preparation is left as .prepare-* and was never sent to ImgDrive.
   writeFile(prepared / "disc.cue", ownedCue);
   writeFile(prepared / "mount.state", manifest.str());
   writeFile(prepared / "prepared", id);
   std::filesystem::rename(prepared, finalDirectory);
   return load(finalDirectory);
}

void MountStateStore::validate(const MountRecord& record) const {
   if (!validId(record.id)) {
      throw std::runtime_error("invalid mount ID");
   }
   const auto actual = load(root / ("mount-" + record.id));
   if (actual.cuePath != record.cuePath || actual.sourcePath != record.sourcePath ||
       actual.drive != record.drive || actual.cueText != record.cueText) {
      throw std::runtime_error("mount record does not match owned cache");
   }
}

void MountStateStore::mark(const MountRecord& record, MountState state) {
   validate(record);
   const auto path = record.cuePath.parent_path() / mountStateName(state);
   if (std::filesystem::exists(std::filesystem::symlink_status(path))) {
      if (readFile(path) != record.id) {
         throw std::runtime_error("invalid existing phase marker");
      }
      return;
   }
   // A partial marker is intentionally treated as uncertain state, never deleted.
   writeFile(path, record.id);
}

void MountStateStore::removeDetached(const MountRecord& record) {
   validate(record);
   const auto directory = root / ("mount-" + record.id);
   if (load(directory).state != MountState::Detached) {
      throw std::runtime_error("cannot clean a mount without confirmed detachment");
   }
   std::set<std::string> allowed{"mount.state", "disc.cue"};
   for (const auto phase : PHASES) {
      allowed.insert(mountStateName(phase));
   }
   for (const auto& entry : std::filesystem::directory_iterator(directory)) {
      if (!allowed.contains(entry.path().filename().string()) || !isRegular(entry.path())) {
         throw std::runtime_error("unrecognized cache contents; nothing deleted");
      }
   }
   std::filesystem::remove(directory / "disc.cue");
   for (const auto phase : PHASES) {
      if (phase != MountState::Detached) {
         std::filesystem::remove(directory / mountStateName(phase));
      }
   }
   std::filesystem::remove(directory / "mount.state");
   std::filesystem::remove(directory / "detached");
   std::filesystem::remove(directory);
}

}
