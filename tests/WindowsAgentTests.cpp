#include "flaccd/mount/WindowsAgentIpc.h"
#include "flaccd/PathText.h"

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
         return flaccd::runWindowsAgent(arguments[2], arguments[4], arguments[6]);
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
      const std::filesystem::path self(module.data());
      const auto call = [&](const std::vector<std::string>& request) {
         return flaccd::requestWindowsAgent(request, executable, cache, self, tag);
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
      require(call({"agent-stop"}).exitCode == 0, "IPC agent stop");
      require(call({"status"}).exitCode == 0, "agent restart");
      require(call({"agent-stop"}).exitCode == 0, "restarted agent stop");
      std::cout << "Windows current-user IPC, concurrent clients, single instance, restart tests passed\n";
      // Leave only this isolated test cache for inspection if a late process
      // still holds its log. Never recursively remove a live agent's resources.
      return 0;
   } catch (const std::exception& error) {
      std::cerr << error.what() << '\n';
      return 1;
   }
}
