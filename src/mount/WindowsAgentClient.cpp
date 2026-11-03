#include "flaccd/mount/WindowsAgentClient.h"
#include "flaccd/mount/WindowsImgDriveHost.h"
#include "flaccd/mount/AgentCommand.h"
#include "flaccd/PathText.h"

#include <windows.h>

#include <array>
#include <stdexcept>
#include <utility>

namespace flaccd {

AgentReply runWindowsAgentClient(std::vector<std::string> arguments, AgentClientSettings settings) {
   arguments = normalizeAgentCommand(std::move(arguments));
   if (agentStopTimeout(arguments)) {
      // Stopping must work with ImgDrive missing/unregistered and after the
      // sibling agent executable was moved. Never discover/start it here.
      return requestWindowsAgent(arguments, {}, {}, {}, settings.endpointTag);
   }
   if (!arguments.empty() && arguments[0] == "mount" && arguments.size() > 1 && !arguments[1].starts_with("--")) {
      arguments[1] = pathToUtf8(std::filesystem::absolute(pathFromUtf8(arguments[1])));
   }
   for (std::size_t position = 1; position < arguments.size(); ++position) {
      if ((arguments[position] == "--imgdrive" || arguments[position] == "--cache-dir") && position + 1 < arguments.size()) {
         const auto option = arguments[position++];
         const auto absolute = std::filesystem::absolute(pathFromUtf8(arguments[position]));
         arguments[position] = pathToUtf8(absolute);
         if (option == "--imgdrive") {
            settings.imgDrive = absolute;
         } else {
            settings.cache = absolute;
         }
      }
   }
   settings.imgDrive = std::filesystem::absolute(findImgDrive(settings.imgDrive));
   settings.cache = settings.cache.empty() ? defaultMountCache() : settings.cache;
   if (settings.agentExecutable.empty()) {
      std::array<wchar_t, 32768> module{};
      const auto count = GetModuleFileNameW(nullptr, module.data(), static_cast<DWORD>(module.size()));
      if (count == 0 || count >= module.size()) {
         throw std::runtime_error("cannot locate flaccd-agent.exe beside this executable");
      }
      settings.agentExecutable = std::filesystem::path(module.data()).parent_path() / L"flaccd-agent.exe";
   }
   return requestWindowsAgent(arguments,
                              settings.imgDrive,
                              settings.cache,
                              settings.agentExecutable,
                              settings.endpointTag);
}

}
