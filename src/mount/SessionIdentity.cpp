#include "flaccd/mount/SessionIdentity.h"

namespace flaccd {

std::string makeSessionIdentity(std::string_view logonSid, std::uint64_t logonTime) {
   if (logonSid.empty() || logonSid.find('@') != std::string_view::npos ||
       logonSid.find('\0') != std::string_view::npos || logonTime == 0) {
      return {};
   }
   return std::string(logonSid) + '@' + std::to_string(logonTime);
}

}
