#include "flaccd/mount/RestoreStore.h"
#include "flaccd/PathText.h"

#include <algorithm>
#include <charconv>
#include <iomanip>
#include <limits>
#include <random>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace flaccd {
namespace {

constexpr std::size_t MAX_DESIRED_MOUNTS = 99;

std::vector<std::pair<std::uint64_t, std::filesystem::path>> snapshots(const std::filesystem::path& root) {
   std::vector<std::pair<std::uint64_t, std::filesystem::path>> result;
   if (!std::filesystem::exists(root)) {
      return result;
   }
   if (!std::filesystem::is_directory(std::filesystem::symlink_status(root))) {
      throw std::runtime_error("restore directory must be a real directory");
   }
   for (const auto& entry : std::filesystem::directory_iterator(root)) {
      const auto name = pathToUtf8(entry.path().filename());
      if (!name.starts_with("desired-") || !name.ends_with(".state")) {
         continue;
      }
      const auto number = std::string_view(name).substr(8, name.size() - 14);
      std::uint64_t generation = 0;
      const auto parsed = std::from_chars(number.data(), number.data() + number.size(), generation, 16);
      if (number.size() != 16 || parsed.ec != std::errc{} || parsed.ptr != number.data() + number.size() ||
          !std::filesystem::is_regular_file(std::filesystem::symlink_status(entry.path()))) {
         throw std::runtime_error("invalid restore snapshot; preserved");
      }
      result.emplace_back(generation, entry.path());
   }
   std::sort(result.begin(), result.end());
   return result;
}

void validate(const RestoreState& state) {
   if (state.mounts.size() > MAX_DESIRED_MOUNTS || state.startupCommand.size() > 32768 ||
       state.previousStartupCommand.size() > 32768 || state.startupCommand.find('\0') != std::string::npos ||
       state.previousStartupCommand.find('\0') != std::string::npos) {
      throw std::invalid_argument("restore state exceeds limits");
   }
   std::vector<std::filesystem::path> sources;
   for (const auto& entry : state.mounts) {
      if (!entry.source.is_absolute() || entry.drive < 'D' || entry.drive > 'Z' || entry.deviceIdentity.empty() ||
          (entry.layout != "auto" && entry.layout != "native" && entry.layout != "text") ||
          entry.mountId.empty() || entry.session.empty() || pathToUtf8(entry.source).find('\0') != std::string::npos ||
          entry.mountId.find('\0') != std::string::npos || entry.session.find('\0') != std::string::npos ||
          entry.deviceIdentity.find('\0') != std::string::npos ||
          std::find(sources.begin(), sources.end(), entry.source) != sources.end()) {
         throw std::invalid_argument("invalid or duplicate desired mount");
      }
      sources.push_back(entry.source);
   }
}

}

RestoreStore::RestoreStore(std::filesystem::path cache) : root(std::move(cache) / "restore") {
}

bool RestoreStore::hasConfiguration() const {
   return !snapshots(root).empty();
}

RestoreState RestoreStore::read() const {
   const auto files = snapshots(root);
   RestoreState state;
   if (files.empty()) {
      return state;
   }
   std::istringstream input(readMountCue(files.back().second));
   std::string magic;
   std::getline(input, magic);
   std::size_t count = 0;
   input >> state.enabled >> std::quoted(state.startupCommand) >> std::quoted(state.previousStartupCommand) >> count;
   if (magic != "FlacCD desired mounts v1" || !input || count > MAX_DESIRED_MOUNTS) {
      throw std::runtime_error("invalid desired-mount state; automatic restoration refused");
   }
   for (std::size_t index = 0; index < count; ++index) {
      DesiredMount entry;
      std::string source;
      input >> std::quoted(source) >> entry.layout >> entry.drive >> std::quoted(entry.deviceIdentity) >>
         entry.noCopy >> entry.mountId >> std::quoted(entry.session) >> entry.suspended;
      entry.source = pathFromUtf8(source);
      state.mounts.push_back(std::move(entry));
   }
   if (!input) {
      throw std::runtime_error("truncated desired-mount state; automatic restoration refused");
   }
   input >> std::ws;
   if (!input.eof()) {
      throw std::runtime_error("unexpected desired-mount state data");
   }
   validate(state);
   return state;
}

void RestoreStore::save(const RestoreState& state) {
   validate(state);
   MountStateStore managed(root.parent_path()); // Establish/verify ownership before adding restore data.
   const auto files = snapshots(root);
   if (!files.empty() && files.back().first == std::numeric_limits<std::uint64_t>::max()) {
      throw std::runtime_error("restore generation overflow");
   }
   std::filesystem::create_directories(root);
   const auto generation = files.empty() ? 1 : files.back().first + 1;
   std::ostringstream name;
   name << "desired-" << std::hex << std::setw(16) << std::setfill('0') << generation << ".state";
   std::ostringstream text;
   text << "FlacCD desired mounts v1\n" << state.enabled << ' ' << std::quoted(state.startupCommand) << ' '
        << std::quoted(state.previousStartupCommand) << ' ' << state.mounts.size() << '\n';
   for (const auto& entry : state.mounts) {
      text << std::quoted(pathToUtf8(entry.source)) << ' ' << entry.layout << ' ' << entry.drive << ' '
           << std::quoted(entry.deviceIdentity) << ' ' << entry.noCopy << ' ' << entry.mountId << ' '
           << std::quoted(entry.session) << ' ' << entry.suspended << '\n';
   }
   if (text.str().size() > 1024 * 1024) {
      throw std::invalid_argument("desired-mount snapshot is too large");
   }
   const auto temporary = root / (".pending-" + name.str() + "-" + std::to_string(std::random_device{}()));
   MountFileSystem filesAdapter;
   const auto error = filesAdapter.createCueExclusive(temporary, text.str());
   if (error) {
      throw std::system_error(error, "create desired-mount snapshot");
   }
   publishMountMetadata(temporary, root / name.str());
   // Keep a previous complete generation for inspection, never as a fallback.
   for (std::size_t index = 0; index + 1 < files.size(); ++index) {
      try {
         const auto previous = readMountCue(files[index].second);
         if (previous.starts_with("FlacCD desired mounts v1\n")) {
            std::error_code error;
            std::filesystem::remove(files[index].second, error);
         }
      } catch (const std::exception&) {
         // Publishing succeeded. Preserve unreadable/foreign older files.
      }
   }
}

void RestoreStore::remember(const MountRecord& record, std::string_view layout, bool noCopy, const std::string& session) {
   auto state = read();
   if (!state.enabled) {
      return;
   }
   if (session.empty()) {
      throw std::runtime_error("cannot remember mount: login session identity unavailable");
   }
   std::erase_if(state.mounts, [&](const auto& entry) { return entry.source == record.sourcePath; });
   state.mounts.push_back({record.sourcePath, std::string(layout), record.actualDrive, record.deviceIdentity,
                           noCopy, record.id, session, false});
   save(state);
}

void RestoreStore::forget(const MountRecord& record) {
   auto state = read();
   const auto removed = std::erase_if(state.mounts, [&](const auto& entry) { return entry.mountId == record.id; });
   if (removed) {
      save(state);
   }
}

void RestoreStore::observedDetach(const MountRecord& record, const std::string& session) {
   auto state = read();
   bool changed = false;
   for (auto entry = state.mounts.begin(); entry != state.mounts.end();) {
      if (entry->mountId == record.id && !entry->suspended && !session.empty() && entry->session == session) {
         entry = state.mounts.erase(entry); // Same login: user eject, not reboot loss.
         changed = true;
      } else {
         if (entry->mountId == record.id && session.empty()) {
            entry->suspended = true;
            changed = true;
         }
         ++entry;
      }
   }
   if (changed) {
      save(state);
   }
}

void RestoreStore::suspend() {
   auto state = read();
   bool changed = false;
   for (auto& entry : state.mounts) {
      changed = changed || !entry.suspended;
      entry.suspended = true;
   }
   if (changed) {
      save(state);
   }
}

}
