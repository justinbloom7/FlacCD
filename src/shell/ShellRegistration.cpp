#include "flaccd/shell/ShellRegistration.h"
#include "flaccd/mount/ProcessRunner.h"

#include <stdexcept>

namespace flaccd {

std::wstring shellMountCommand(std::wstring_view executable) {
   const bool absolute = executable.size() > 3 &&
      ((executable[1] == L':' && executable[2] == L'\\') || executable.starts_with(L"\\\\"));
   const auto extension = executable.size() >= 4 ? executable.substr(executable.size() - 4) : L"";
   const bool executableExtension = extension.size() == 4 && extension[0] == L'.' &&
      (extension[1] == L'e' || extension[1] == L'E') &&
      (extension[2] == L'x' || extension[2] == L'X') &&
      (extension[3] == L'e' || extension[3] == L'E');
   if (!absolute || executable.find_first_of(L"\"\r\n") != std::wstring_view::npos ||
       executable.find(L'\0') != std::wstring_view::npos || !executableExtension) {
      throw std::invalid_argument("Shell registration requires an absolute Windows executable path");
   }
   // Escape ShellExecute template percent signs, independently of CRT quoting.
   // No cmd.exe or environment-expanding registry value is involved.
   std::wstring literal;
   for (const auto character : executable) {
      literal += character;
      if (character == L'%') {
         literal += L'%';
      }
   }
   return quoteWindowsArgument(literal) + L" mount \"%1\"";
}

ShellValues shellRegistration(std::wstring_view executable) {
   return {{{L"", L""}, L"Mount as Audio CD"},
           {{L"", L"MultiSelectModel"}, L"Single"},
           {{L"", L"FlacCD.Owner"}, L"FlacCD.Shell.v1"},
           {{L"", L"FlacCD.Executable"}, std::wstring(executable)},
           {{L"command", L""}, shellMountCommand(executable)}};
}

std::string runShellCommand(ShellRegistry& registry, std::string_view action, std::wstring_view executable) {
   if (action != "install" && action != "uninstall" && action != "status") {
      throw std::invalid_argument("Usage: flaccd shell install|uninstall|status");
   }
   const auto current = registry.read();
   if (!current) {
      if (action == "install") {
         registry.create(shellRegistration(executable));
         return "Installed per-user FLAC command: Mount as Audio CD.\n";
      }
      return "FlacCD Explorer command is not installed.\n";
   }
   const auto path = current->find({L"", L"FlacCD.Executable"});
   bool owned = false;
   if (path != current->end()) {
      try {
         owned = *current == shellRegistration(path->second);
      } catch (const std::invalid_argument&) {
         owned = false;
      }
   }
   if (!owned) {
      throw std::runtime_error("Explorer registration conflicts with an unowned or modified entry; preserved");
   }
   if (action == "uninstall") {
      registry.remove(*current);
      return "Removed FlacCD Explorer command; file associations unchanged.\n";
   }
   if (action == "install" && path->second != executable) {
      throw std::runtime_error("Explorer command belongs to another FlacCD location; run shell uninstall before reinstalling");
   }
   return "FlacCD Explorer command is installed (shell status does not check ImgDrive).\n";
}

}
