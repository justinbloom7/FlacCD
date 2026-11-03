#include "flaccd/mount/WindowsAgentClient.h"
#include "flaccd/mount/WindowsImgDriveHost.h"
#include "flaccd/PathText.h"
#include "flaccd/shell/WindowsShellRegistry.h"

#include <windows.h>

#include <iostream>

namespace {

void printUsage() {
   std::cout << "Usage:\n"
             << "  flaccd mount FILE.flac [--drive D:|auto] [--layout native|text|auto] [--no-copy]\n"
             << "  flaccd unmount D:\n"
             << "  flaccd status\n"
             << "  flaccd drives\n"
             << "  flaccd agent stop [--timeout SECONDS]\n"
             << "  flaccd shell install|uninstall|status\n"
             << "Options (after command):\n"
             << "  --imgdrive PATH.exe    Installed executable (or FLACCD_IMGDRIVE)\n"
             << "  --cache-dir DIR        Persistent cache; default LocalAppData/FlacCD/ImgDrive/v1\n"
             << "  --timeout SECONDS      1..600; default 30; total deadline for agent stop\n"
             << "Other commands start a per-user agent as needed; agent stop never starts one.\n"
             << "Auto selects an identified, visible, empty ImgDrive device. Physical drives are refused.\n"
             << "Mounting reuses/creates an adjacent CUE, else stages a hard link or compressed copy.\n"
             << "--no-copy forbids compressed copies. No BIN is generated.\n"
             << "External eject cleanup normally takes 5..10 seconds after empty media is confirmed.\n"
             << "Unknown/missing devices and uncertain mounts retain resources for inspection.\n"
             << "agent stop retains mounts; agent-stop is an alias. No service/admin rights.\n"
             << "Changing the running agent's executable/cache requires agent stop first.\n";
   std::cout << "shell install registers Mount as Audio CD for this user without changing the FLAC default.\n"
             << "Run from the final deployment folder; shell uninstall removes the registration.\n"
             << "Windows 11 may show the command under Show more options.\n";
}

int run(std::vector<std::string> arguments) {
   if (arguments.empty() || (arguments.size() == 1 && arguments[0] == "--help")) {
      printUsage();
      return arguments.empty() ? 2 : 0;
   }
   if (arguments[0] == "shell") {
      if (arguments.size() != 2) {
         throw std::invalid_argument("Usage: flaccd shell install|uninstall|status");
      }
      flaccd::WindowsShellRegistry registry;
      std::cout << flaccd::runShellCommand(registry, arguments[1], flaccd::shellExecutablePath());
      return 0;
   }
   const auto reply = flaccd::runWindowsAgentClient(std::move(arguments));
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
