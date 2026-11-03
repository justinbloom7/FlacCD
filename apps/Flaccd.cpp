#include "flaccd/mount/WindowsAgentClient.h"
#include "flaccd/mount/WindowsImgDriveHost.h"
#include "flaccd/PathText.h"
#include "flaccd/shell/WindowsShellRegistry.h"
#include "flaccd/mount/WindowsRestore.h"

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
             << "  flaccd restore enable|disable|status\n"
             << "  flaccd restore run [--timeout SECONDS]  (1..300; default 60)\n"
             << "  flaccd restore forget FILE.flac\n"
             << "Options (after command):\n"
             << "  --imgdrive PATH.exe    Installed executable (or FLACCD_IMGDRIVE)\n"
             << "  --cache-dir DIR        Persistent cache; default LocalAppData/FlacCD/ImgDrive/v1\n"
             << "  --timeout SECONDS      1..600; default 30; total deadline for agent stop\n"
             << "Mount/unmount/status/drives start a per-user agent as needed; agent stop never starts one.\n"
             << "Restore configuration does not start an agent; restore run uses it only when needed.\n"
             << "Auto selects an identified, visible, empty ImgDrive device. Physical drives are refused.\n"
             << "Mounting reuses/creates an adjacent CUE, else stages a hard link or compressed copy.\n"
             << "With no embedded CUE, mounting requires a matching adjacent .cue referencing the FLAC.\n"
             << "--no-copy forbids compressed copies. No BIN is generated.\n"
             << "External eject cleanup normally takes 5..10 seconds after empty media is confirmed.\n"
             << "Unknown/missing devices and uncertain mounts retain resources for inspection.\n"
             << "agent stop retains mounts; agent-stop is an alias. No service/admin rights.\n"
             << "Changing the running agent's executable/cache requires agent stop first.\n";
   std::cout << "shell install registers Mount as Audio CD for this user without changing the FLAC default.\n"
             << "Run from the final deployment folder; shell uninstall removes the registration.\n"
             << "Windows 11 may show the command under Show more options.\n";
}

void showExplorerError(const std::string& message) {
   MessageBoxW(nullptr, flaccd::pathFromUtf8(message).c_str(), L"FlacCD - Mount as Audio CD",
               MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
}

int run(std::vector<std::string> arguments, bool explorer) {
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
   if (arguments[0] == "restore") {
      const auto reply = flaccd::runWindowsRestore(arguments);
      std::cout << reply.output;
      std::cerr << reply.diagnostics;
      return static_cast<int>(reply.exitCode);
   }
   const auto reply = flaccd::runWindowsAgentClient(std::move(arguments));
   std::cout << reply.output;
   std::cerr << reply.diagnostics;
   if (explorer && reply.exitCode != 0) {
      showExplorerError(reply.diagnostics.empty() ? reply.output : reply.diagnostics);
   }
   return static_cast<int>(reply.exitCode);
}

}

int wmain(int argumentCount, wchar_t* arguments[]) {
   SetConsoleOutputCP(CP_UTF8);
   bool explorer = false;
   try {
      std::vector<std::string> text;
      for (int position = 1; position < argumentCount; ++position) {
         if (position >= 3 && std::wstring_view(arguments[position]) == L"--explorer" &&
             std::wstring_view(arguments[1]) == L"mount") {
            explorer = true;
            continue;
         }
         text.push_back(flaccd::pathToUtf8(std::filesystem::path(arguments[position])));
      }
      return run(std::move(text), explorer);
   } catch (const std::invalid_argument& error) {
      std::cerr << "flaccd: " << error.what() << '\n';
      if (explorer) {
         showExplorerError(error.what());
      }
      return 2;
   } catch (const std::exception& error) {
      std::cerr << "flaccd: " << error.what() << '\n';
      if (explorer) {
         showExplorerError(error.what());
      }
      return 1;
   }
}
