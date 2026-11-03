#include <windows.h>

#include <filesystem>
#include <fstream>
#include <string>

// Deliberately independent of FlacCD and its agent; record the UTF-16 filename
// received through ShellExecute, without mounting or invoking a command shell.
int wmain(int count, wchar_t* arguments[]) {
   if (count != 4 || std::wstring(arguments[1]) != L"mount" || std::wstring(arguments[3]) != L"--explorer") {
      return 2;
   }
   wchar_t executable[32768];
   const DWORD length = GetModuleFileNameW(nullptr, executable, 32768);
   if (length == 0 || length >= 32768) {
      return 3;
   }
   std::ofstream output(std::filesystem::path(std::wstring(executable, length) + L".args"), std::ios::binary);
   const std::wstring argument(arguments[2]);
   output.write(reinterpret_cast<const char*>(argument.data()),
                static_cast<std::streamsize>(argument.size() * sizeof(wchar_t)));
   return output ? 0 : 4;
}
