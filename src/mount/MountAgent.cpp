#include "flaccd/mount/MountAgent.h"
#include "flaccd/PathText.h"

#include <charconv>
#include <set>

namespace flaccd {
namespace {

char parseDrive(std::string_view value) {
   if (value.size() != 2 || value[1] != ':') {
      throw std::invalid_argument("drive must be D: through Z:");
   }
   const auto letter = value[0] >= 'a' && value[0] <= 'z' ? value[0] - 'a' + 'A' : value[0];
   if (letter < 'D' || letter > 'Z') {
      throw std::invalid_argument("drive must be D: through Z:");
   }
   return static_cast<char>(letter);
}

std::string mediaName(MediaState media) {
   switch (media) {
   case MediaState::Empty: return "empty";
   case MediaState::Ready: return "ready";
   case MediaState::Missing: return "missing";
   case MediaState::Unknown: return "uncertain";
   }
   return "uncertain";
}

}

MountAgent::MountAgent(ProcessRunner& processHost, std::filesystem::path imageExecutable, std::filesystem::path mountCache)
   : host(processHost), executable(std::move(imageExecutable)), cache(std::move(mountCache)) {
}

AgentReply MountAgent::handle(const std::vector<std::string>& arguments) {
   std::lock_guard guard(mutex);
   AgentReply reply;
   try {
      if (arguments.empty()) {
         throw std::invalid_argument("missing command");
      }
      const auto& command = arguments[0];
      if (command != "mount" && command != "unmount" && command != "status" && command != "drives" && command != "agent-stop") {
         throw std::invalid_argument("unknown command");
      }
      std::size_t position = 1;
      std::filesystem::path source;
      std::optional<char> drive;
      if (command == "mount" || command == "unmount") {
         if (position == arguments.size()) {
            throw std::invalid_argument("missing source/drive");
         }
         if (command == "mount") {
            source = pathFromUtf8(arguments[position++]);
            if (!source.is_absolute()) {
               throw std::invalid_argument("IPC mount source must be absolute");
            }
         } else {
            drive = parseDrive(arguments[position++]);
         }
      }
      bool noCopy = false;
      std::string layout = "auto";
      std::chrono::seconds timeout(30);
      std::set<std::string> seen;
      while (position < arguments.size()) {
         const auto option = arguments[position++];
         if (!seen.insert(option).second) {
            throw std::invalid_argument("duplicate option " + option);
         }
         if (option == "--no-copy" && command == "mount") {
            noCopy = true;
            continue;
         }
         if (position == arguments.size()) {
            throw std::invalid_argument("missing option value: " + option);
         }
         const auto& value = arguments[position++];
         if (option == "--drive" && command == "mount") {
            if (value != "auto") {
               drive = parseDrive(value);
            }
         } else if (option == "--layout" && command == "mount") {
            if (value != "auto" && value != "native" && value != "text") {
               throw std::invalid_argument("--layout must be native|text|auto");
            }
            layout = value;
         } else if (option == "--timeout") {
            unsigned count = 0;
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), count);
            if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || count < 1 || count > 600) {
               throw std::invalid_argument("--timeout must be 1..600 seconds");
            }
            timeout = std::chrono::seconds(count);
         } else if (option == "--cache-dir" || option == "--imgdrive") {
            const auto expected = option == "--cache-dir" ? cache : executable;
            if (std::filesystem::weakly_canonical(pathFromUtf8(value)) != std::filesystem::weakly_canonical(expected)) {
               throw std::invalid_argument("agent configuration differs; run flaccd agent-stop, then retry with the desired options");
            }
         } else {
            throw std::invalid_argument("unknown/inapplicable option " + option);
         }
      }
      if (command == "agent-stop") {
         reply.output = "Agent stopping; mount resources retained for recovery.\n";
         return reply;
      }
      ImgDriveBackend backend(host, executable, cache, timeout, nullptr, [&](const std::string& text) {
         if (reply.diagnostics.size() < 65536) {
            reply.diagnostics += text.substr(0, 65536 - reply.diagnostics.size()) + "\n";
         }
      });
      if (command == "mount") {
         const auto record = backend.mount(source, drive, layout, {}, noCopy);
         reply.output = "Mounted on " + std::string(1, record.actualDrive) + ":\nCUE: " + pathToUtf8(record.cuePath) + "\n";
      } else if (command == "unmount") {
         reply.output = backend.unmount(*drive);
      } else if (command == "drives") {
         reply.output = backend.drives();
      }
      const auto statuses = backend.status();
      for (const auto& status : statuses) {
         const auto& record = status.record;
         const auto requested = std::string(1, record.drive);
         const auto actual = record.actualDrive ? std::string(1, record.actualDrive) : "unbound";
         const auto media = mediaName(status.media);
         reply.records.push_back({record.id, requested, actual, record.deviceIdentity, mountStateName(record.state),
                                  mountStrategyName(record), pathToUtf8(record.sourcePath), pathToUtf8(record.cuePath),
                                  pathToUtf8(record.audioPath), media, record.imgDriveSelector});
         if (command == "status") {
            reply.output += "Requested " + requested + ": actual " + actual + ": " + mountStateName(record.state) +
                            " " + media + " identity=" + record.deviceIdentity + " selector=" + record.imgDriveSelector + "\n  " + mountStrategyName(record) +
                            "\n  FLAC: " + pathToUtf8(record.sourcePath) + "\n  CUE: " + pathToUtf8(record.cuePath) + "\n";
         }
      }
      if (command == "status" && statuses.empty()) {
         reply.output = "No managed mounts.\n";
      }
   } catch (const std::invalid_argument& error) {
      reply.exitCode = 2;
      reply.diagnostics += std::string(error.what()) + "\n";
   } catch (const std::exception& error) {
      reply.exitCode = 1;
      reply.diagnostics += std::string(error.what()) + "\n";
   }
   return reply;
}

std::string MountAgent::reconcile(std::chrono::steady_clock::time_point now) {
   std::lock_guard guard(mutex);
   std::string result;
   try {
      ImgDriveBackend backend(host, executable, cache);
      std::set<std::string> active;
      const auto drives = host.opticalDrives();
      for (const auto& status : backend.status()) {
         const auto& record = status.record;
         active.insert(record.id);
         const auto* actual = findDevice(drives, record.deviceIdentity);
         // An unmounting marker records intent, not successful vendor execution.
         // A failed/uncertain unmount must remain available for explicit retry.
         const bool confirmed = record.deviceConfirmed &&
                                (record.state == MountState::Mounted || record.state == MountState::Detached);
         if (!confirmed || !actual || !actual->imgDrive || actual->media != MediaState::Empty) {
            emptySince.erase(record.id);
            continue;
         }
         const auto [entry, inserted] = emptySince.try_emplace(record.id, now);
         // Notifications trigger inspection, not deletion. Two independent empty
         // observations at least five seconds apart debounce transient readiness.
         if (!inserted && now - entry->second >= std::chrono::seconds(5)) {
            try {
               result += backend.cleanupEjected(record);
               emptySince.erase(record.id);
            } catch (const std::exception& error) {
               result += std::string("Eject cleanup deferred: ") + error.what() + "\n";
            }
         }
      }
      for (auto entry = emptySince.begin(); entry != emptySince.end();) {
         if (!active.contains(entry->first)) {
            entry = emptySince.erase(entry);
         } else {
            ++entry;
         }
      }
   } catch (const std::exception& error) {
      result += std::string("Reconciliation deferred: ") + error.what() + "\n";
   }
   return result;
}

}
