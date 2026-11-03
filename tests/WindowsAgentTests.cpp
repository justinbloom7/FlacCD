#include "flaccd/mount/WindowsAgentIpc.h"
#include "flaccd/PathText.h"
#include "flaccd/mount/WindowsImgDriveHost.h"

#include <windows.h>

#include <array>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {

void require(bool condition, const char* message) {
   if (!condition) {
      throw std::runtime_error(message);
   }
}

}

int wmain(int count, wchar_t* arguments[]) {
   try {
      // The test executable doubles as an isolated agent host. Production never
      // uses this namespace and no test sends a mount/eject command to ImgDrive.
      if (count == 7 && std::wstring_view(arguments[1]) == L"--imgdrive") {
         const auto result = flaccd::runWindowsAgent(arguments[2], arguments[4], arguments[6]);
         if (std::wstring_view(arguments[6]).ends_with(L"-slow")) {
            Sleep(5000); // Test a delayed process exit, never force-terminate it.
         }
         return result;
      }
      std::array<wchar_t, 32768> module{};
      const auto length = GetModuleFileNameW(nullptr, module.data(), static_cast<DWORD>(module.size()));
      require(length != 0 && length < module.size(), "test module pathname");
      const auto tag = L"test-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
      const auto directory = std::filesystem::temp_directory_path() / tag;
      std::filesystem::create_directory(directory);
      const auto executable = directory / L"ImgDrive mock.exe";
      { std::ofstream file(executable, std::ios::binary); file << "MZfixture"; }
      const auto cache = directory / L"cache 音楽";
      require(count == 2, "expected isolated CLI probe executable");
      const std::filesystem::path cliProbe(arguments[1]);
      const auto agentCopy = directory / L"Agent copy.exe";
      std::filesystem::copy_file(std::filesystem::path(module.data()), agentCopy);
      flaccd::WindowsImgDriveHost host;
      const auto cli = [&](const std::vector<std::string>& request) {
         std::vector<std::string> parameters{flaccd::pathToUtf8(std::filesystem::path(tag)),
                                             flaccd::pathToUtf8(executable), flaccd::pathToUtf8(cache),
                                             flaccd::pathToUtf8(agentCopy)};
         parameters.insert(parameters.end(), request.begin(), request.end());
         return host.run(cliProbe, parameters, std::chrono::seconds(40));
      };
      auto absent = cli({"agent", "stop"});
      require(absent.exitCode == 0 && absent.diagnostics.find("not running") != std::string::npos,
              "CLI stop of absent agent must succeed without starting it");
      require(!std::filesystem::exists(cache), "absent stop created cache/started agent");
      require(cli({"agent", "stop", "--imgdrive", "missing-backend.exe"}).exitCode == 0,
              "absent stop must not require an installed backend");
      const auto started = cli({"status"});
      require(started.exitCode == 0 && !started.timedOut, "CLI agent startup");
      const auto call = [&](const std::vector<std::string>& request) {
         return flaccd::requestWindowsAgent(request, executable, cache, agentCopy, tag);
      };
      std::array<flaccd::AgentReply, 2> replies;
      std::array<std::exception_ptr, 2> errors;
      std::array<std::thread, 2> clients;
      for (std::size_t index = 0; index < clients.size(); ++index) {
         clients[index] = std::thread([&, index] {
            try {
               replies[index] = call({"status"});
            } catch (...) {
               errors[index] = std::current_exception();
            }
         });
      }
      for (auto& client : clients) {
         client.join();
      }
      for (const auto& error : errors) {
         if (error) {
            std::rethrow_exception(error);
         }
      }
      require(replies[0].exitCode == 0 && replies[1].exitCode == 0, "concurrent real named-pipe status requests");
      // The live endpoint must survive a duplicate server attempt unchanged.
      require(flaccd::runWindowsAgent(executable, cache, tag) == 0, "single-instance mutex");
      const auto bad = call({"mount", "relative 音楽.flac"});
      require(bad.exitCode == 2, "structured IPC invalid mount request");
      require(call({"drives"}).exitCode == 0, "IPC optical enumeration");
      require(call({"unmount", "D:"}).exitCode == 1, "unmanaged unmount must be refused");
      const auto stopped = cli({"agent", "stop", "--timeout", "10"});
      require(stopped.exitCode == 0 && !stopped.timedOut && stopped.diagnostics.find("Agent stopped") != std::string::npos,
              "CLI stop must confirm complete process exit");
      const auto writable = CreateFileW(agentCopy.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
      require(writable != INVALID_HANDLE_VALUE, "agent image remains locked after successful stop");
      CloseHandle(writable);
      require(cli({"agent", "stop"}).exitCode == 0, "repeated CLI stop");
      require(call({"agent", "stop"}).output.find("not running") != std::string::npos, "direct IPC stop started absent agent");
      require(!std::filesystem::exists(cache / "mount.state"), "stop created unexpected mount state");
      require(cli({"status"}).exitCode == 0, "agent restart");
      require(cli({"agent-stop"}).exitCode == 0, "legacy stop alias");
      const auto slowTag = tag + L"-slow";
      const auto slowCache = directory / L"slow-cache";
      const auto slowStart = flaccd::requestWindowsAgent({"status"}, executable, slowCache, agentCopy, slowTag);
      require(slowStart.exitCode == 0, "slow-exit helper startup");
      const auto timeout = flaccd::requestWindowsAgent({"agent", "stop", "--timeout", "1"}, {}, {}, {}, slowTag);
      require(timeout.exitCode == 1 && timeout.diagnostics.find("did not exit") != std::string::npos,
              "shutdown timeout falsely reported success");
      const auto pidStart = timeout.diagnostics.find("Agent process ");
      require(pidStart != std::string::npos, "timeout should identify the process to inspect");
      const auto pid = std::stoul(timeout.diagnostics.substr(pidStart + 14));
      const auto slowProcess = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
      require(slowProcess != nullptr, "slow process was unexpectedly force-terminated");
      const auto exited = WaitForSingleObject(slowProcess, 10000);
      CloseHandle(slowProcess);
      require(exited == WAIT_OBJECT_0, "slow helper failed to finish naturally");
      std::cout << "Windows current-user IPC, concurrent clients, single instance, restart tests passed\n";
      // Leave only this isolated test cache for inspection if a late process
      // still holds its log. Never recursively remove a live agent's resources.
      return 0;
   } catch (const std::exception& error) {
      std::cerr << error.what() << '\n';
      return 1;
   }
}
