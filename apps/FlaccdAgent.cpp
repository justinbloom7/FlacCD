#include "flaccd/mount/WindowsAgentIpc.h"
#include "flaccd/mount/WindowsImgDriveHost.h"

#include <iostream>
#include <stdexcept>

int wmain(int argumentCount, wchar_t* arguments[]) {
   try {
      std::filesystem::path executable;
      std::filesystem::path cache;
      for (int position = 1; position < argumentCount; ++position) {
         const std::wstring option(arguments[position]);
         if (position + 1 == argumentCount) {
            throw std::invalid_argument("agent option requires a value");
         }
         if (option == L"--imgdrive") {
            executable = arguments[++position];
         } else if (option == L"--cache-dir") {
            cache = arguments[++position];
         } else {
            throw std::invalid_argument("unknown agent option");
         }
      }
      executable = std::filesystem::absolute(flaccd::findImgDrive(executable));
      cache = cache.empty() ? flaccd::defaultMountCache() : std::filesystem::absolute(cache);
      return flaccd::runWindowsAgent(executable, cache);
   } catch (const std::exception& error) {
      std::cerr << "flaccd-agent: " << error.what() << '\n';
      return 1;
   }
}
