#include "flaccd/CueSheetWriter.h"
#include "flaccd/PathText.h"

#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace flaccd {
namespace {

constexpr std::uint64_t CUE_SECTOR_BYTES = 2352;

std::string quoteValue(std::string_view value) {
   for (const unsigned char character : value) {
      if (character < 32 || character == 127 || character == '"') {
         throw std::invalid_argument("CUE text/filename contains a quote or control character");
      }
   }
   return "\"" + std::string(value) + "\"";
}

std::string timestamp(std::uint64_t sector) {
   std::ostringstream output;
   output.imbue(std::locale::classic());
   output << std::setfill('0') << std::setw(2) << sector / (60 * SECTORS_PER_SECOND)
          << ':' << std::setw(2) << (sector / SECTORS_PER_SECOND) % 60
          << ':' << std::setw(2) << sector % SECTORS_PER_SECOND;
   return output.str();
}

// Duplicate identical tags are harmless; conflicting values cannot be chosen
// without a policy. Selected CUE descriptions always take precedence.
std::string uniqueTag(const FlacMetadata& metadata, std::string_view name) {
   std::string result;
   for (const auto& comment : metadata.vorbisComments) {
      const auto separator = comment.find('=');
      auto key = comment.substr(0, separator);
      for (auto& character : key) {
         if (character >= 'a' && character <= 'z') {
            character = static_cast<char>(character - 'a' + 'A');
         }
      }
      if (separator == std::string::npos || key != name) {
         continue;
      }
      const auto value = comment.substr(separator + 1);
      if (!result.empty() && !value.empty() && value != result) {
         throw std::invalid_argument("conflicting Vorbis values for " + std::string(name));
      }
      if (!value.empty()) {
         result = value;
      }
   }
   return result;
}

void writeDescription(std::ostream& output,
                      std::string_view indent,
                      std::string_view command,
                      const std::string& value) {
   if (!value.empty()) {
      output << indent << command << ' ' << quoteValue(value) << '\n';
   }
}

std::string makeCue(const TrackLayout& layout,
                    const FlacMetadata& metadata,
                    const std::string& sourceName,
                    std::string_view sourceType) {
   auto title = layout.getTitle();
   auto performer = layout.getPerformer();
   if (title.empty()) {
      title = uniqueTag(metadata, "ALBUM");
   }
   if (performer.empty()) {
      performer = uniqueTag(metadata, "ALBUMARTIST");
      if (performer.empty()) {
         performer = uniqueTag(metadata, "ARTIST");
      }
   }
   std::ostringstream output;
   output.imbue(std::locale::classic());
   writeDescription(output, "", "TITLE", title);
   writeDescription(output, "", "PERFORMER", performer);
   if (!layout.getCatalogNumber().empty()) {
      output << "CATALOG " << layout.getCatalogNumber() << '\n';
   }
   output << "FILE " << quoteValue(sourceName) << ' ' << sourceType << '\n';
   std::uint32_t previousTrack = 0;
   for (const auto& track : layout.getTracks()) {
      if (previousTrack != 0 && track.number != previousTrack + 1) {
         throw std::invalid_argument("external CUE requires consecutive track numbers");
      }
      previousTrack = track.number;
      output << "   TRACK " << std::setfill('0') << std::setw(2) << track.number << " AUDIO\n";
      writeDescription(output, "      ", "TITLE", track.title);
      writeDescription(output, "      ", "PERFORMER", track.performer);
      if (!track.isrc.empty()) {
         output << "      ISRC " << track.isrc << '\n';
      }
      if (track.flags.preEmphasis || track.flags.digitalCopyPermitted.value_or(false)) {
         output << "      FLAGS";
         if (track.flags.preEmphasis) {
            output << " PRE";
         }
         if (track.flags.digitalCopyPermitted.value_or(false)) {
            output << " DCP";
         }
         output << '\n';
      }
      for (const auto& index : track.indices) {
         if (!index.sourceSector) {
            throw std::invalid_argument("cannot export an index without source audio");
         }
         output << "      INDEX " << std::setw(2) << index.number << ' '
                << timestamp(*index.sourceSector) << '\n';
      }
   }
   return output.str();
}

std::uint64_t validateSource(const TrackLayout& layout, const FlacMetadata& metadata) {
   const auto sectors = layout.getLeadOut().sourceSector;
   if (sectors == 0 || sectors > std::numeric_limits<std::uint64_t>::max() / CUE_SECTOR_BYTES ||
       metadata.sampleRate != 44100 || metadata.channels != 2 || metadata.bitsPerSample != 16 ||
       metadata.totalSamples != sectors * SAMPLES_PER_SECTOR) {
      throw std::invalid_argument("CUE requires a complete, sector-aligned CDDA source");
   }
   for (const auto& region : layout.getRegions()) {
      if (region.kind != RegionKind::SourceAudio || !region.sourceStartSector) {
         throw std::invalid_argument("CUE cannot represent source-less PREGAP/POSTGAP audio in this mode");
      }
   }
   return sectors;
}

}

std::string generateCueSheet(const TrackLayout& layout,
                             const FlacMetadata& metadata,
                             const std::string& binName) {
   if (binName.empty() || binName.find('/') != std::string::npos) {
      throw std::invalid_argument("referenced BIN must be a nonempty filename without directory components");
   }
   if (binName.find_first_of("\\:*?<>|") != std::string::npos) {
      throw std::invalid_argument("BIN filename contains characters unsupported by Windows paths");
   }
   const auto sectorCount = validateSource(layout, metadata);
   const auto cue = makeCue(layout, metadata, binName, "BINARY");
   const auto roundTrip = TrackLayout::fromBinaryCue(sectorCount * CUE_SECTOR_BYTES,
                                                    parseCueTextMetadata(cue));
   if (!compareTrackLayouts(layout, roundTrip).empty()) {
      throw std::invalid_argument("generated CUE does not preserve the selected layout");
   }
   return cue;
}

std::string generateFlacCueSheet(const TrackLayout& layout,
                                 const FlacMetadata& metadata,
                                 const std::filesystem::path& sourcePath) {
   validateSource(layout, metadata);
   const auto source = std::filesystem::canonical(sourcePath);
   if (!std::filesystem::is_regular_file(source)) {
      throw std::invalid_argument("FLAC source must be a regular file");
   }
   auto sourceName = pathToUtf8(source);
   // Some Windows standard libraries return extended canonical path prefixes.
   // CUE consumers expect ordinary drive/UNC paths, not Win32 device syntax.
   if (sourceName.starts_with("\\\\?\\UNC\\")) {
      sourceName = "\\\\" + sourceName.substr(8);
   } else if (sourceName.starts_with("\\\\?\\") && sourceName.size() >= 7 && sourceName[5] == ':') {
      sourceName.erase(0, 4);
   }
   if (sourceName.find_first_of("*?<>|") != std::string::npos) {
      throw std::invalid_argument("FLAC path contains characters unsupported by Windows paths");
   }
   const auto cue = makeCue(layout, metadata, sourceName, "WAVE");
   const auto parsed = parseCueTextMetadata(cue);
   const auto roundTrip = TrackLayout::fromText(metadata, parsed);
   if (!compareTrackLayouts(layout, roundTrip).empty() ||
       !std::filesystem::equivalent(pathFromUtf8(parsed.fileName), source)) {
      throw std::invalid_argument("FLAC-backed CUE does not preserve layout or source identity");
   }
   return cue;
}

}
