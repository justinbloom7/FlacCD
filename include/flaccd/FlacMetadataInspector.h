#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace flaccd {

struct CueIndex {
   std::uint8_t number = 0;
   std::uint64_t offset = 0; // Sample frames relative to the track offset.
};

struct CueTrack {
   std::uint8_t number = 0;
   std::uint64_t offset = 0; // Sample frames relative to the FLAC stream start.
   std::string isrc;
   bool isAudio = true;
   bool preEmphasis = false;
   std::vector<CueIndex> indices;
};

struct NativeCueSheet {
   std::string catalogNumber;
   std::uint64_t leadIn = 0;
   bool isCd = false;
   std::vector<CueTrack> tracks; // Includes the lead-out entry.
};

struct FlacMetadata {
   std::uint32_t sampleRate = 0;
   std::uint32_t channels = 0;
   std::uint32_t bitsPerSample = 0;
   std::uint64_t totalSamples = 0; // Zero means unknown, as defined by FLAC.
   std::vector<NativeCueSheet> cueSheets;
   std::vector<std::string> embeddedCueSheets; // Unparsed CUESHEET values, in file order.
   // Preserve all Vorbis comments, including duplicate tags, as name=value strings.
   std::vector<std::string> vorbisComments;
};

class FlacMetadataInspector {
public:
   // Reads metadata only. Throws std::runtime_error on unreadable or malformed metadata.
   // Does not validate audio frames or enforce CD-DA compatibility.
   FlacMetadata inspect(const std::filesystem::path& path) const;
};

}
