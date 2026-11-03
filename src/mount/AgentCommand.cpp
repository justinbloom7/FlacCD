#include "flaccd/mount/AgentCommand.h"

#include <charconv>
#include <set>
#include <stdexcept>

namespace flaccd {

std::vector<std::string> normalizeAgentCommand(std::vector<std::string> arguments) {
   if (!arguments.empty() && arguments[0] == "agent") {
      if (arguments.size() < 2 || arguments[1] != "stop") {
         throw std::invalid_argument("use flaccd agent stop [--timeout SECONDS]");
      }
      arguments[0] = "agent-stop";
      arguments.erase(arguments.begin() + 1);
   }
   return arguments;
}

std::optional<std::chrono::milliseconds> agentStopTimeout(const std::vector<std::string>& arguments) {
   if (arguments.empty() || arguments[0] != "agent-stop") {
      return std::nullopt;
   }
   std::chrono::milliseconds timeout = std::chrono::seconds(30);
   std::set<std::string> seen;
   for (std::size_t position = 1; position < arguments.size();) {
      const auto& option = arguments[position++];
      if (!seen.insert(option).second || position == arguments.size()) {
         throw std::invalid_argument("duplicate stop option or missing value: " + option);
      }
      const auto& value = arguments[position++];
      if (option == "--timeout") {
         unsigned seconds = 0;
         const auto result = std::from_chars(value.data(), value.data() + value.size(), seconds);
         if (result.ec != std::errc{} || result.ptr != value.data() + value.size() || seconds < 1 || seconds > 600) {
            throw std::invalid_argument("stop --timeout must be 1..600 seconds");
         }
         timeout = std::chrono::seconds(seconds);
      } else if (option != "--imgdrive" && option != "--cache-dir") {
         throw std::invalid_argument("unknown stop option: " + option);
      }
      // Legacy configuration options are accepted but do not select another
      // agent: stop always addresses the existing per-user endpoint only.
   }
   return timeout;
}

}
