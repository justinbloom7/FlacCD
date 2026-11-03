#include "flaccd/mount/WindowsAgentClient.h"
#include "flaccd/PathText.h"

#include <iostream>
#include <stdexcept>
#include <utility>

int wmain(int count, wchar_t* arguments[]) {
   try {
      if (count < 6) {
         throw std::invalid_argument("expected private endpoint, backend, cache, agent path, and command");
      }
      flaccd::AgentClientSettings settings;
      settings.endpointTag = arguments[1];
      settings.imgDrive = arguments[2];
      settings.cache = arguments[3];
      settings.agentExecutable = arguments[4];
      std::vector<std::string> command;
      for (int position = 5; position < count; ++position) {
         command.push_back(flaccd::pathToUtf8(std::filesystem::path(arguments[position])));
      }
      const auto reply = flaccd::runWindowsAgentClient(std::move(command), std::move(settings));
      std::cout << reply.output;
      std::cerr << reply.diagnostics;
      return static_cast<int>(reply.exitCode);
   } catch (const std::exception& error) {
      std::cerr << error.what() << '\n';
      return 1;
   }
}
