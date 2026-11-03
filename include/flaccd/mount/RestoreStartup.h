#pragma once

#include <optional>
#include <string>

namespace flaccd {

class RestoreStartup {
public:
   virtual ~RestoreStartup() = default;
   virtual std::optional<std::wstring> read() = 0;
   virtual void write(const std::wstring& command) = 0;
   virtual void remove() = 0;
};

std::wstring restoreStartupCommand(const std::wstring& executable,
                                   const std::wstring& cache,
                                   const std::wstring& imgDrive);
void updateRestoreStartup(RestoreStartup& startup,
                           const std::wstring& ownedCommand,
                           const std::optional<std::wstring>& replacement);

}
