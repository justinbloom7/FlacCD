#include "flaccd/mount/RestoreStartup.h"
#include "flaccd/mount/ProcessRunner.h"
#include "flaccd/mount/RestoreStore.h"
#include "flaccd/PathText.h"

#include <stdexcept>

namespace flaccd {

std::wstring restoreStartupCommand(const std::wstring& executable,
                                   const std::wstring& cache,
                                   const std::wstring& imgDrive) {
   auto command = quoteWindowsArgument(executable) + L" restore run --startup --cache-dir " +
                  quoteWindowsArgument(cache);
   if (!imgDrive.empty()) {
      command += L" --imgdrive " + quoteWindowsArgument(imgDrive);
   }
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

void refreshRestoreStartup(RestoreStartup& startup,
                            RestoreStore& store,
                            const std::wstring& executable,
                            const std::wstring& cache,
                            const std::wstring& imgDrive) {
   const auto previous = store.read();
   if (!previous.enabled) {
      return;
   }
   const auto current = startup.read();
   const auto owned = pathFromUtf8(previous.startupCommand).wstring();
   const auto prior = pathFromUtf8(previous.previousStartupCommand).wstring();
   if (current && *current != owned && *current != prior) {
      throw std::runtime_error("foreign/modified restore startup registration; preserved");
   }
   const auto command = restoreStartupCommand(executable, cache, imgDrive);
   if (current == command && owned == command) {
      return;
   }
   auto updated = previous;
   updated.previousStartupCommand = previous.startupCommand;
   updated.startupCommand = pathToUtf8(std::filesystem::path(command));
   store.save(updated);
   try {
      updateRestoreStartup(startup, current.value_or(owned), command);
   } catch (...) {
      store.save(previous);
      throw;
   }
}

}
