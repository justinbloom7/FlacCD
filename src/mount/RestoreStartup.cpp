#include "flaccd/mount/RestoreStartup.h"
#include "flaccd/mount/ProcessRunner.h"

#include <stdexcept>

namespace flaccd {

std::wstring restoreStartupCommand(const std::wstring& executable,
                                   const std::wstring& cache,
                                   const std::wstring& imgDrive) {
   auto command = quoteWindowsArgument(executable) + L" restore run --startup --cache-dir " +
                  quoteWindowsArgument(cache) + L" --imgdrive " + quoteWindowsArgument(imgDrive);
   // Microsoft's documented Run value limit. Never silently truncate a path.
   if (command.size() > 260) {
      throw std::invalid_argument("login command exceeds Windows Run's 260-character limit; use shorter installation/cache paths");
   }
   return command;
}

void updateRestoreStartup(RestoreStartup& startup,
                           const std::wstring& ownedCommand,
                           const std::optional<std::wstring>& replacement) {
   const auto current = startup.read();
   if (current && *current != ownedCommand && (!replacement || *current != *replacement)) {
      throw std::runtime_error("foreign/modified restore startup registration; preserved");
   }
   if (replacement) {
      startup.write(*replacement);
   } else {
      startup.remove();
   }
}

}
