#include "flaccd/mount/WindowsAgentIpc.h"
#include "flaccd/mount/WindowsImgDriveHost.h"
#include "flaccd/PathText.h"

#include <windows.h>

#include <array>
#include <iostream>

namespace {

void printUsage() {
   std::cout << "Usage:\n"
             << "  flaccd mount FILE.flac [--drive D:|auto] [--layout native|text|auto] [--no-copy]\n"
             << "  flaccd unmount D:\n"
             << "  flaccd status\n"
             << "  flaccd drives\n"
             << "  flaccd agent-stop\n"
             << "Options (after command):\n"
             << "  --imgdrive PATH.exe    Installed executable (or FLACCD_IMGDRIVE)\n"
             << "  --cache-dir DIR        Persistent cache; default LocalAppData/FlacCD/ImgDrive/v1\n"
             << "  --timeout SECONDS      Per-phase timeout, 1..600; default 30\n"
             << "A per-user background agent starts automatically; no service/admin rights.\n"
             << "Auto selects an identified, visible, empty ImgDrive device. Physical drives are refused.\n"
             << "Mounting reuses/creates an adjacent CUE, else stages a hard link or compressed copy.\n"
             << "--no-copy forbids compressed copies. No BIN is generated.\n"
             << "External eject cleanup normally takes 5..10 seconds after empty media is confirmed.\n"
             << "Unknown/missing devices and uncertain mounts retain resources for inspection.\n"
             << "agent-stop retains mounts; the next command restarts/reconciles the agent.\n"
             << "Changing the running agent's executable/cache requires agent-stop first.\n";
}

int run(std::vector<std::string> arguments) {
   if (arguments.empty() || (arguments.size() == 1 && arguments[0] == "--help")) {
      printUsage();
      return arguments.empty() ? 2 : 0;
   }
   std::filesystem::path executable;
   std::filesystem::path cache;
   if (arguments[0] == "mount" && arguments.size() > 1 && !arguments[1].starts_with("--")) {
      arguments[1] = flaccd::pathToUtf8(std::filesystem::absolute(flaccd::pathFromUtf8(arguments[1])));
   }
   for (std::size_t position = 1; position < arguments.size(); ++position) {
      if ((arguments[position] == "--imgdrive" || arguments[position] == "--cache-dir") && position + 1 < arguments.size()) {
         const auto option = arguments[position++];
         const auto absolute = std::filesystem::absolute(flaccd::pathFromUtf8(arguments[position]));
         arguments[position] = flaccd::pathToUtf8(absolute);
         if (option == "--imgdrive") {
            executable = absolute;
         } else {
            cache = absolute;
         }
      }
   }
   executable = std::filesystem::absolute(flaccd::findImgDrive(executable));
   cache = cache.empty() ? flaccd::defaultMountCache() : cache;
   std::array<wchar_t, 32768> module{};
   const auto count = GetModuleFileNameW(nullptr, module.data(), static_cast<DWORD>(module.size()));
   if (count == 0 || count >= module.size()) {
      throw std::runtime_error("cannot locate flaccd-agent.exe beside this executable");
   }
   const auto agent = std::filesystem::path(module.data()).parent_path() / L"flaccd-agent.exe";
   const auto reply = flaccd::requestWindowsAgent(arguments, executable, cache, agent);
   std::cout << reply.output;
   std::cerr << reply.diagnostics;
   return static_cast<int>(reply.exitCode);
}

}

int wmain(int argumentCount, wchar_t* arguments[]) {
   SetConsoleOutputCP(CP_UTF8);
   try {
      std::vector<std::string> text;
      for (int position = 1; position < argumentCount; ++position) {
         text.push_back(flaccd::pathToUtf8(std::filesystem::path(arguments[position])));
      }
      return run(std::move(text));
   } catch (const std::invalid_argument& error) {
      std::cerr << "flaccd: " << error.what() << '\n';
      return 2;
   } catch (const std::exception& error) {
      std::cerr << "flaccd: " << error.what() << '\n';
      return 1;
   }
}
