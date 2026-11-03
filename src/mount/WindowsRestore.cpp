#include "flaccd/mount/WindowsRestore.h"
#include "flaccd/mount/RestoreCoordinator.h"
#include "flaccd/mount/WindowsAgentClient.h"
#include "flaccd/mount/WindowsImgDriveHost.h"
#include "flaccd/shell/WindowsShellRegistry.h"
#include "flaccd/PathText.h"

#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <charconv>
#include <fstream>
#include <memory>
#include <set>
#include <sstream>
#include <system_error>

namespace flaccd {

AgentReply runWindowsRestore(const std::vector<std::string>& arguments) {
   if (arguments.size() < 2) {
      throw std::invalid_argument("Usage: flaccd restore enable|disable|status|run|forget FILE");
   }
   const auto& action = arguments[1];
   if (action != "enable" && action != "disable" && action != "status" && action != "run" &&
       action != "forget" && action != "refresh" && action != "pause") {
      throw std::invalid_argument("unknown restore action");
   }
   AgentClientSettings settings;
   settings.cache = defaultMountCache();
   std::filesystem::path forgotten;
   std::size_t position = 2;
   if (action == "forget") {
      if (position == arguments.size()) {
         throw std::invalid_argument("restore forget requires a FLAC path");
      }
      forgotten = std::filesystem::weakly_canonical(pathFromUtf8(arguments[position++]));
   }
   unsigned seconds = 60;
   bool startup = false;
   std::set<std::string> seen;
   while (position < arguments.size()) {
      const auto option = arguments[position++];
      if (!seen.insert(option).second) {
         throw std::invalid_argument("duplicate restore option");
      }
      if (option == "--startup" && action == "run") {
         startup = true;
         continue;
      }
      if (position == arguments.size()) {
         throw std::invalid_argument("missing restore option value");
      }
      const auto& value = arguments[position++];
      if (option == "--cache-dir") {
         settings.cache = std::filesystem::absolute(pathFromUtf8(value));
      } else if (option == "--imgdrive") {
         settings.imgDrive = std::filesystem::absolute(pathFromUtf8(value));
      } else if (option == "--timeout" && (action == "run" || action == "pause")) {
         const auto parsed = std::from_chars(value.data(), value.data() + value.size(), seconds);
         if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || seconds < 1 || seconds > 300) {
            throw std::invalid_argument("restore --timeout must be 1..300 seconds");
         }
      } else {
         throw std::invalid_argument("unknown/inapplicable restore option: " + option);
      }
   }
   WindowsImgDriveHost host;
   WindowsRestoreStartup registration;
   auto configurationLock = host.lock(defaultMountCache() / "restore-registration", std::chrono::seconds(10));
   if (!configurationLock) {
      throw std::runtime_error("restoration registration is busy");
   }
   // Recognize a prior executable location only when the configured cache
   // independently owns the exact startup command. This permits upgrades to
   // migrate a development-copy startup entry without adopting foreign values.
   if (!seen.contains("--cache-dir")) {
      const auto startupCommand = registration.read();
      if (startupCommand) {
         int count = 0;
         auto values = CommandLineToArgvW(startupCommand->c_str(), &count);
         std::unique_ptr<wchar_t*, decltype(&LocalFree)> valuesOwner(values, &LocalFree);
         if (values) {
            if (count == 8 &&
                std::wstring_view(values[1]) == L"restore" && std::wstring_view(values[2]) == L"run" &&
                std::wstring_view(values[3]) == L"--startup" && std::wstring_view(values[4]) == L"--cache-dir" &&
                std::wstring_view(values[6]) == L"--imgdrive") {
               const auto candidateCache = std::filesystem::path(values[5]);
               bool owned = candidateCache.is_absolute() && _wcsicmp(values[0], shellExecutablePath().c_str()) == 0;
               if (!owned && candidateCache.is_absolute()) {
                  const auto candidate = RestoreStore(candidateCache).read();
                  const auto text = pathToUtf8(std::filesystem::path(*startupCommand));
                  owned = text == candidate.startupCommand || text == candidate.previousStartupCommand;
               }
               if (owned) {
                  settings.cache = candidateCache;
                  if (!seen.contains("--imgdrive")) {
                     settings.imgDrive = values[7];
                  }
               }
            }
         }
      }
   }
   settings.cache = std::filesystem::weakly_canonical(settings.cache);
   RestoreStore store(settings.cache);
   AgentReply reply;
   if (action != "run") {
      auto lock = host.lock(settings.cache, std::chrono::seconds(10));
      if (!lock) {
         throw std::runtime_error("cannot acquire restoration configuration lock");
      }
      MountStateStore managed(settings.cache); // Verify cache ownership before writing config.
      const auto previous = store.read();
      auto state = previous;
      const auto current = registration.read();
      if (action == "pause") {
         store.suspend();
         lock.reset(); // Let an in-flight helper finish its current agent request.
         auto drained = host.lock(settings.cache / "restore-helper", std::chrono::seconds(seconds));
         if (!drained) {
            throw std::runtime_error("restoration helper has not exited; maintenance must wait");
         }
         reply.output = "Restoration paused and helper drained; mounts/resources retained.\n";
         return reply;
      }
      if (action == "status") {
         reply.output = std::string("Restoration ") + (state.enabled ? "enabled" : "disabled") +
                        "; login registration " + (!current ? "absent" :
                        *current == pathFromUtf8(state.startupCommand).wstring() ? "owned" : "foreign/modified") + "\n";
         if (current) {
            reply.output += "Login command: " + pathToUtf8(std::filesystem::path(*current)) + "\n";
         }
         reply.output += "Desired mounts: " + std::to_string(state.mounts.size()) + "\n";
         reply.output += "Restore log: " + pathToUtf8(settings.cache / "restore.log") + "\n";
         if (!state.enabled) {
            reply.output += "Use restore enable from the installed copy to enable login restoration.\n";
         } else if (state.mounts.empty()) {
            reply.output += "Mount a disc after enabling restoration so it can be remembered.\n";
         }
         for (const auto& entry : state.mounts) {
            reply.output += std::string(entry.suspended ? "Suspended: " : "Desired: ") + pathToUtf8(entry.source) + "\n";
         }
         return reply;
      }
      if (action == "forget") {
         std::erase_if(state.mounts, [&](const auto& entry) {
            std::error_code error;
            return entry.source == forgotten || std::filesystem::equivalent(entry.source, forgotten, error);
         });
         store.save(state);
         reply.output = "Restore intent removed for the specified path; active media untouched.\n";
         return reply;
      }
      if (action == "refresh") {
         if (state.enabled) {
            if (settings.imgDrive.empty()) {
               settings.imgDrive = std::filesystem::absolute(findImgDrive());
            }
            refreshRestoreStartup(registration, store, shellExecutablePath(), settings.cache.wstring(), settings.imgDrive.wstring());
            captureDesiredMounts(host, settings.cache, store, false);
         }
         reply.output = "Restoration maintenance refreshed; only confirmed live mounts rearmed.\n";
         return reply;
      }
      if (current && *current != pathFromUtf8(previous.startupCommand).wstring() &&
          *current != pathFromUtf8(previous.previousStartupCommand).wstring()) {
         throw std::runtime_error("restore startup belongs to another/modified registration; preserved");
      }
      if (action == "disable") {
         state.enabled = false;
         store.save(state); // An interrupted removal still leaves restoration disabled.
         updateRestoreStartup(registration, current.value_or(pathFromUtf8(previous.startupCommand).wstring()), {});
         reply.output = "Automatic login restoration disabled; desired list/resources retained.\n";
         return reply;
      }
      settings.imgDrive = std::filesystem::absolute(findImgDrive(settings.imgDrive));
      if (!std::filesystem::is_regular_file(settings.imgDrive)) {
         throw std::runtime_error("ImgDrive executable unavailable; restoration was not enabled");
      }
      const auto command = restoreStartupCommand(shellExecutablePath(), settings.cache.wstring(), settings.imgDrive.wstring());
      state.enabled = true;
      state.previousStartupCommand = previous.startupCommand;
      state.startupCommand = pathToUtf8(std::filesystem::path(command));
      try {
         store.save(state);
         captureDesiredMounts(host, settings.cache, store);
         updateRestoreStartup(registration, current.value_or(pathFromUtf8(previous.startupCommand).wstring()), command);
      } catch (...) {
         store.save(previous);
         throw;
      }
      reply.output = "Automatic login restoration enabled; confirmed current and future mounts are remembered.\n"
                     "Disable ImgDrive's own automount to avoid competing restorers.\n";
      return reply;
   }
   configurationLock.reset();
   if (startup) {
      DWORD processes[2];
      if (GetConsoleProcessList(processes, 2) == 1) {
         ShowWindow(GetConsoleWindow(), SW_HIDE);
      }
   }
   const auto deadline = GetTickCount64() + static_cast<ULONGLONG>(seconds) * 1000;
   // Never hold the mount-state lock while calling the agent.
   auto helperLock = host.lock(settings.cache / "restore-helper", std::chrono::milliseconds(100));
   if (!helperLock) {
      throw std::runtime_error("another restoration helper is already running");
   }
   std::ofstream log;
   {
      auto lock = host.lock(settings.cache, std::chrono::seconds(1));
      if (!lock) {
         throw std::runtime_error("restoration helper is busy");
      }
      MountStateStore managed(settings.cache);
      if (!store.read().enabled) {
         reply.output = "Automatic restoration disabled.\n";
         return reply;
      }
      log.open(settings.cache / "restore.log", std::ios::binary | std::ios::trunc);
   }
   if (!log) {
      throw std::runtime_error("cannot write restore.log in the managed cache");
   }
   std::size_t logBytes = 0;
   const auto appendLog = [&](const std::string& message) {
      const auto count = std::min<std::size_t>(message.size(), 1024 * 1024 - logBytes);
      log.write(message.data(), static_cast<std::streamsize>(count));
      log.flush();
      logBytes += count;
   };
   for (;;) {
      const auto now = GetTickCount64();
      if (now >= deadline) {
         break;
      }
      const auto remaining = deadline - now;
      {
         auto lock = host.lock(settings.cache, std::chrono::milliseconds(std::min<ULONGLONG>(1000, remaining)));
         if (!lock) {
            reply.exitCode = 1;
            reply.diagnostics = "Restore state is busy; no further startup request submitted.\n";
            appendLog(reply.diagnostics);
            return reply;
         }
         const auto state = store.read();
         if (!state.enabled || std::all_of(state.mounts.begin(), state.mounts.end(), [](const auto& entry) {
            return entry.suspended;
         })) {
            reply.exitCode = state.enabled && !state.mounts.empty() ? 1 : 0;
            reply.output = "Restoration disabled, empty, or suspended; helper exiting.\n";
            appendLog(reply.output);
            return reply;
         }
      }
      try {
         // Discovery can legitimately precede ImgDrive installation/readiness.
         settings.imgDrive = std::filesystem::absolute(findImgDrive(settings.imgDrive));
      } catch (const std::exception& problem) {
         reply.exitCode = 3;
         reply.diagnostics = problem.what();
         appendLog(reply.diagnostics + '\n');
         Sleep(static_cast<DWORD>(std::min<ULONGLONG>(1000, remaining)));
         continue;
      }
      const auto submittedAt = GetTickCount64();
      if (submittedAt >= deadline) {
         break;
      }
      const auto remainingBudget = std::chrono::milliseconds(static_cast<std::int64_t>(deadline - submittedAt));
      const auto attemptTimeout = restoreAttemptTimeout(remainingBudget);
      if (!attemptTimeout) {
         break;
      }
      settings.requestTimeout = remainingBudget;
      try {
         appendLog("Restore attempt: vendor/TOC timeout=" + std::to_string(attemptTimeout->count()) +
                   " seconds; helper budget=" + std::to_string(remainingBudget.count()) + " ms\n");
         reply = runWindowsAgentClient({"restore-step", "--timeout", std::to_string(attemptTimeout->count())}, settings);
      } catch (const std::exception& problem) {
         // Submission outcome may be uncertain: never replay after transport loss.
         reply.exitCode = 1;
         reply.diagnostics = std::string("Restore IPC failed; no automatic replay: ") + problem.what();
      }
      appendLog(reply.output + reply.diagnostics + '\n');
      if (reply.exitCode != 3) {
         return reply;
      }
      const auto after = GetTickCount64();
      Sleep(static_cast<DWORD>(std::min<ULONGLONG>(1000, after < deadline ? deadline - after : 0)));
   }
   reply.exitCode = 1;
   reply.diagnostics += "\nRestoration deadline reached; pending desired mounts retained for a future login.\n";
   appendLog(reply.diagnostics);
   return reply;
}

}
