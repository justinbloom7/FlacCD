#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace flaccd {

struct TextCueIndex {
   std::uint32_t number = 0;
   std::uint64_t sourceSector = 0;
};

struct CueTrackMetadata {
   std::uint32_t number = 0;
   std::string title;
   std::string performer;
   std::string type;
   std::string isrc;
   bool preEmphasis = false;
   bool digitalCopyPermitted = false;
   std::vector<TextCueIndex> indices;
   std::optional<std::uint64_t> pregap;
   std::optional<std::uint64_t> postgap;
};

struct CueTextMetadata {
   std::string title;
   std::string performer;
   std::vector<CueTrackMetadata> tracks;
   std::string fileName;
   std::string fileType;
   std::string catalogNumber;
   std::vector<std::string> errors;
};

// Exact mm:ss:ff conversion (75 sectors/second); throws std::invalid_argument
// for malformed fields and overflow. Minutes may have more than two digits.
std::uint64_t parseCueTimestamp(std::string_view timestamp);

// Single shared parser for descriptive and layout metadata. Descriptive callers
// may inspect partial results; layout construction rejects every recorded error.
// Command/type/flag names are case-insensitive; UTF-8 values are retained verbatim.
CueTextMetadata parseCueTextMetadata(std::string_view text);

}
