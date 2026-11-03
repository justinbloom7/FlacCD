#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace flaccd {

// A logon SID alone can repeat after reboot. Pair it with the immutable logon
// timestamp, not a periodically estimated boot time affected by clock changes.
std::string makeSessionIdentity(std::string_view logonSid, std::uint64_t logonTime);

}
