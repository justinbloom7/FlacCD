#include "flaccd/TrackLayout.h"

#include <algorithm>
#include <limits>
#include <iterator>
#include <stdexcept>
#include <utility>

namespace flaccd {
namespace {

void require(bool condition, const std::string& message) {
   if (!condition) {
      throw std::invalid_argument(message);
   }
}

std::uint64_t checkedAdd(std::uint64_t first, std::uint64_t second) {
   require(second <= std::numeric_limits<std::uint64_t>::max() - first, "position overflow");
   return first + second;
}

std::int64_t toLba(std::uint64_t position) {
   require(position <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()),
           "disc position exceeds signed LBA range");
   return static_cast<std::int64_t>(position);
}

std::uint64_t sampleToSector(std::uint64_t sample) {
   require(sample % SAMPLES_PER_SECTOR == 0, "sample position is not sector-aligned");
   return sample / SAMPLES_PER_SECTOR;
}

std::uint64_t sourceLength(const FlacMetadata& metadata) {
   require(metadata.sampleRate == 44100 && metadata.channels == 2 && metadata.bitsPerSample == 16,
           "layout requires 44100 Hz stereo 16-bit audio; conversion is unsupported");
   require(metadata.totalSamples > 0, "unknown or empty source length");
   return sampleToSector(metadata.totalSamples);
}

void validateIsrc(const std::string& isrc) {
   require(isrc.empty() || (isrc.size() == 12 &&
           std::all_of(isrc.begin(), isrc.end(), [](char character) {
              return (character >= 'A' && character <= 'Z') ||
                     (character >= '0' && character <= '9');
           })), "ISRC must contain 12 uppercase alphanumeric characters");
}

void validateCatalog(const std::string& catalog) {
   require(catalog.empty() || (catalog.size() == 13 &&
           std::all_of(catalog.begin(), catalog.end(), [](char character) {
              return character >= '0' && character <= '9';
           })), "CD catalog must contain 13 digits");
}

}

TrackLayout TrackLayout::fromNative(const FlacMetadata& metadata, const NativeCueSheet& cueSheet) {
   const auto totalSectors = sourceLength(metadata);
   require(cueSheet.isCd, "non-CD native CUESHEET is unsupported");
   require(cueSheet.leadIn >= 2 * 44100 && cueSheet.leadIn % SAMPLES_PER_SECTOR == 0,
           "native lead-in must be sector-aligned and at least two seconds");
   require(cueSheet.tracks.size() >= 2 && cueSheet.tracks.size() <= 100,
           "native CUESHEET requires audio tracks and a final lead-out");
   const auto& leadOutTrack = cueSheet.tracks.back();
   require(leadOutTrack.number == 170 && leadOutTrack.indices.empty(),
           "native lead-out must be final track 170 with no indexes");
   require(sampleToSector(leadOutTrack.offset) == totalSectors,
           "native lead-out conflicts with STREAMINFO source length");
   validateCatalog(cueSheet.catalogNumber);
   std::vector<LayoutTrack> tracks;
   for (std::size_t position = 0; position + 1 < cueSheet.tracks.size(); ++position) {
      const auto& source = cueSheet.tracks[position];
      require(source.isAudio, "data/mixed-mode tracks are unsupported");
      require(!source.indices.empty() && source.indices.front().offset == 0,
              "native track must begin at its first index (relative offset zero)");
      sampleToSector(source.offset);
      LayoutTrack track;
      track.number = source.number;
      track.isrc = source.isrc;
      track.flags.preEmphasis = source.preEmphasis;
      for (const auto& index : source.indices) {
         sampleToSector(index.offset);
         const auto absoluteSample = checkedAdd(source.offset, index.offset);
         track.indices.push_back({index.number, 0, sampleToSector(absoluteSample), IndexOrigin::Metadata});
      }
      tracks.push_back(std::move(track));
   }
   auto result = normalize(metadata, std::move(tracks));
   result.leadOut.fromNativeMetadata = true;
   result.leadInSamples = cueSheet.leadIn;
   result.catalogNumber = cueSheet.catalogNumber;
   return result;
}

TrackLayout TrackLayout::fromText(const FlacMetadata& metadata, const CueTextMetadata& cueSheet) {
   require(cueSheet.errors.empty(), cueSheet.errors.empty() ? "" : "text CUE: " + cueSheet.errors.front());
   require(!cueSheet.fileName.empty() && (cueSheet.fileType == "WAVE" || cueSheet.fileType == "FLAC"),
           "text CUE requires one FILE of type WAVE or FLAC");
   validateCatalog(cueSheet.catalogNumber);
   std::vector<LayoutTrack> tracks;
   std::optional<std::uint32_t> previousNumber;
   for (const auto& source : cueSheet.tracks) {
      require(source.type == "AUDIO", "data/mixed-mode tracks are unsupported");
      require(!previousNumber || source.number == *previousNumber + 1,
              "text CUE track numbers must be consecutive");
      previousNumber = source.number;
      LayoutTrack track;
      track.number = source.number;
      track.title = source.title;
      track.performer = source.performer.empty() ? cueSheet.performer : source.performer;
      track.isrc = source.isrc;
      track.flags = {source.preEmphasis, source.digitalCopyPermitted};
      track.declaredPregapSectors = source.pregap;
      track.declaredPostgapSectors = source.postgap;
      for (const auto& index : source.indices) {
         track.indices.push_back({index.number, 0, index.sourceSector, IndexOrigin::Metadata});
      }
      tracks.push_back(std::move(track));
   }
   auto result = normalize(metadata, std::move(tracks));
   result.catalogNumber = cueSheet.catalogNumber;
   result.title = cueSheet.title;
   result.performer = cueSheet.performer;
   return result;
}

TrackLayout TrackLayout::fromBinaryCue(std::uint64_t byteCount, const CueTextMetadata& cueSheet) {
   constexpr std::uint64_t BYTES_PER_SAMPLE_FRAME = 4;
   constexpr std::uint64_t AUDIO_SECTOR_BYTES = SAMPLES_PER_SECTOR * BYTES_PER_SAMPLE_FRAME;
   require(byteCount > 0 && byteCount % AUDIO_SECTOR_BYTES == 0,
           "BIN length must be a positive multiple of 2352 bytes");
   require(cueSheet.fileType == "BINARY", "external audio CUE requires FILE type BINARY");
   // Reuse the single-file source mapping and all existing layout validation.
   auto sourceCue = cueSheet;
   sourceCue.fileType = "WAVE";
   FlacMetadata source;
   source.sampleRate = 44100;
   source.channels = 2;
   source.bitsPerSample = 16;
   source.totalSamples = byteCount / BYTES_PER_SAMPLE_FRAME;
   return fromText(source, sourceCue);
}

const std::string& TrackLayout::getTitle() const {
   return title;
}

const std::string& TrackLayout::getPerformer() const {
   return performer;
}

TrackLayout TrackLayout::normalize(const FlacMetadata& metadata, std::vector<LayoutTrack> tracks) {
   const auto totalSectors = sourceLength(metadata);
   require(!tracks.empty() && tracks.size() <= 99, "layout requires 1..99 audio tracks");
   std::uint32_t previousNumber = 0;
   for (std::size_t position = 0; position < tracks.size(); ++position) {
      auto& track = tracks[position];
      const auto context = "track " + std::to_string(track.number) + ": ";
      require(track.number > previousNumber && track.number <= 99,
              context + "track numbers must be strictly increasing in 1..99");
      previousNumber = track.number;
      validateIsrc(track.isrc);
      require(!track.indices.empty(), context + "missing INDEX 01");
      require(track.indices.front().number <= 1, context + "first index must be 00 or 01");
      require(track.indices.size() <= 100, context + "too many indexes");
      bool foundIndexOne = false;
      for (std::size_t indexPosition = 0; indexPosition < track.indices.size(); ++indexPosition) {
         const auto& index = track.indices[indexPosition];
         require(index.sourceSector.has_value() && index.number <= 99,
                 context + "invalid source index");
         require(*index.sourceSector < totalSectors, context + "index outside source audio");
         if (indexPosition > 0) {
            const auto& previous = track.indices[indexPosition - 1];
            require(index.number == previous.number + 1, context + "index numbers must be consecutive");
            require(*index.sourceSector > *previous.sourceSector ||
                    (previous.number == 0 && index.number == 1 && index.sourceSector == previous.sourceSector),
                    context + "index positions must increase (00/01 may coincide)");
         }
         if (index.number == 1) {
            foundIndexOne = true;
            if (track.indices.front().number == 0) {
               track.storedPregapSectors = *index.sourceSector - *track.indices.front().sourceSector;
            }
         }
      }
      require(foundIndexOne, context + "missing INDEX 01");
      require(!(track.declaredPregapSectors && track.indices.front().number == 0),
              context + "PREGAP combined with INDEX 00 is ambiguous and unsupported");
      if (position == 0) {
         require(*track.indices.front().sourceSector == 0,
                 context + "unassigned source prefix: first index must start at source sector zero");
      }
      if (position + 1 < tracks.size()) {
         require(!tracks[position + 1].indices.empty(), "next track is missing indexes");
         require(tracks[position + 1].indices.front().sourceSector.has_value(), "next track lacks source offset");
         require(*track.indices.back().sourceSector < *tracks[position + 1].indices.front().sourceSector,
                 context + "track offsets overlap or are out of order");
      }
   }

   TrackLayout result;
   std::uint64_t insertedSectors = 0;
   for (std::size_t position = 0; position < tracks.size(); ++position) {
      auto& track = tracks[position];
      const auto sourceBegin = *track.indices.front().sourceSector;
      const auto sourceEnd = position + 1 == tracks.size() ? totalSectors :
                             *tracks[position + 1].indices.front().sourceSector;
      track.startLba = toLba(checkedAdd(sourceBegin, insertedSectors));
      const auto pregap = track.declaredPregapSectors.value_or(0);
      if (pregap > 0) {
         insertedSectors = checkedAdd(insertedSectors, pregap);
         const auto end = toLba(checkedAdd(sourceBegin, insertedSectors));
         result.regions.push_back({track.startLba, end, track.number, 0, RegionKind::Pregap, std::nullopt});
      }
      for (std::size_t indexPosition = 0; indexPosition < track.indices.size(); ++indexPosition) {
         auto& index = track.indices[indexPosition];
         index.discLba = toLba(checkedAdd(*index.sourceSector, insertedSectors));
         const auto nextSource = indexPosition + 1 == track.indices.size() ? sourceEnd :
                                 *track.indices[indexPosition + 1].sourceSector;
         const auto end = toLba(checkedAdd(nextSource, insertedSectors));
         if (index.discLba < end) {
            result.regions.push_back({index.discLba, end, track.number, index.number,
                                      RegionKind::SourceAudio, index.sourceSector});
         }
      }
      if (pregap > 0) {
         track.indices.insert(track.indices.begin(), {0, track.startLba, std::nullopt, IndexOrigin::PregapDirective});
      }
      const auto postgap = track.declaredPostgapSectors.value_or(0);
      if (postgap > 0) {
         const auto begin = toLba(checkedAdd(sourceEnd, insertedSectors));
         insertedSectors = checkedAdd(insertedSectors, postgap);
         const auto end = toLba(checkedAdd(sourceEnd, insertedSectors));
         result.regions.push_back({begin, end, track.number, std::nullopt, RegionKind::Postgap, std::nullopt});
      }
      track.endLba = toLba(checkedAdd(sourceEnd, insertedSectors));
   }
   const auto& firstIndices = tracks.front().indices;
   const auto firstIndexOne = std::find_if(firstIndices.begin(), firstIndices.end(), [](const auto& index) {
      return index.number == 1;
   });
   const auto origin = firstIndexOne->discLba;
   for (auto& track : tracks) {
      track.startLba -= origin;
      track.endLba -= origin;
      for (auto& index : track.indices) {
         index.discLba -= origin;
      }
   }
   for (auto& region : result.regions) {
      region.beginLba -= origin;
      region.endLba -= origin;
   }
   result.leadOut = {toLba(checkedAdd(totalSectors, insertedSectors)) - origin, totalSectors, false};
   result.tracks = std::move(tracks);
   return result;
}

std::span<const LayoutTrack> TrackLayout::getTracks() const {
   return tracks;
}

std::span<const LayoutRegion> TrackLayout::getRegions() const {
   return regions;
}

const LeadOut& TrackLayout::getLeadOut() const {
   return leadOut;
}

const std::optional<std::uint64_t>& TrackLayout::getLeadInSamples() const {
   return leadInSamples;
}

const std::string& TrackLayout::getCatalogNumber() const {
   return catalogNumber;
}

std::optional<DiscPosition> TrackLayout::queryLba(std::int64_t discLba) const {
   const auto region = std::upper_bound(regions.begin(), regions.end(), discLba,
                                       [](std::int64_t position, const LayoutRegion& candidate) {
                                          return position < candidate.beginLba;
                                       });
   if (region == regions.begin()) {
      return std::nullopt;
   }
   const auto& candidate = *std::prev(region);
   if (discLba >= candidate.endLba) {
      return std::nullopt;
   }
   auto source = candidate.sourceStartSector;
   if (source) {
      *source += static_cast<std::uint64_t>(discLba - candidate.beginLba);
   }
   return DiscPosition{candidate.trackNumber, candidate.indexNumber, candidate.kind, source};
}

std::vector<std::string> compareTrackLayouts(const TrackLayout& first, const TrackLayout& second) {
   std::vector<std::string> differences;
   const auto firstTracks = first.getTracks();
   const auto secondTracks = second.getTracks();
   if (first.getLeadOut().discLba != second.getLeadOut().discLba ||
       first.getLeadOut().sourceSector != second.getLeadOut().sourceSector) {
      differences.push_back("lead-out differs");
   }
   if (!first.getCatalogNumber().empty() && !second.getCatalogNumber().empty() &&
       first.getCatalogNumber() != second.getCatalogNumber()) {
      differences.push_back("catalog numbers differ");
   }
   if (firstTracks.size() != secondTracks.size()) {
      differences.push_back("track counts differ");
      return differences;
   }
   for (std::size_t position = 0; position < firstTracks.size(); ++position) {
      const auto& left = firstTracks[position];
      const auto& right = secondTracks[position];
      const auto context = "track at position " + std::to_string(position + 1) + ": ";
      if (left.number != right.number || left.startLba != right.startLba || left.endLba != right.endLba) {
         differences.push_back(context + "track number or disc boundaries differ");
      }
      if (left.flags.preEmphasis != right.flags.preEmphasis ||
          (left.flags.digitalCopyPermitted && right.flags.digitalCopyPermitted &&
           left.flags.digitalCopyPermitted != right.flags.digitalCopyPermitted)) {
         differences.push_back(context + "known flags differ");
      }
      if (!left.isrc.empty() && !right.isrc.empty() && left.isrc != right.isrc) {
         differences.push_back(context + "ISRC differs");
      }
      if (left.storedPregapSectors != right.storedPregapSectors ||
          left.declaredPregapSectors.value_or(0) != right.declaredPregapSectors.value_or(0) ||
          left.declaredPostgapSectors.value_or(0) != right.declaredPostgapSectors.value_or(0)) {
         differences.push_back(context + "gap mapping differs");
      }
      if (left.indices.size() != right.indices.size()) {
         differences.push_back(context + "index counts differ");
         continue;
      }
      for (std::size_t index = 0; index < left.indices.size(); ++index) {
         const auto& leftIndex = left.indices[index];
         const auto& rightIndex = right.indices[index];
         if (leftIndex.number != rightIndex.number || leftIndex.discLba != rightIndex.discLba ||
             leftIndex.sourceSector != rightIndex.sourceSector || leftIndex.origin != rightIndex.origin) {
            differences.push_back(context + "index positions or source mapping differ");
            break;
         }
      }
   }
   return differences;
}

TrackLayout selectTrackLayout(const FlacMetadata& metadata, std::string_view source) {
   if (source == "native") {
      if (metadata.cueSheets.size() != 1) {
         throw std::invalid_argument("native selection requires exactly one native CUESHEET");
      }
      return TrackLayout::fromNative(metadata, metadata.cueSheets.front());
   }
   if (source == "text") {
      if (metadata.embeddedCueSheets.size() != 1) {
         throw std::invalid_argument("text selection requires exactly one embedded CUESHEET comment");
      }
      return TrackLayout::fromText(metadata,
                                   parseCueTextMetadata(metadata.embeddedCueSheets.front()));
   }
   if (source != "auto") {
      throw std::invalid_argument("layout selection must be native, text, or auto");
   }
   std::vector<TrackLayout> candidates;
   std::string rejected;
   for (const auto& cue : metadata.cueSheets) {
      try {
         candidates.push_back(TrackLayout::fromNative(metadata, cue));
      } catch (const std::invalid_argument& error) {
         rejected += std::string("native: ") + error.what() + "; ";
      }
   }
   const auto nativeCount = candidates.size();
   for (const auto& cue : metadata.embeddedCueSheets) {
      try {
         candidates.push_back(TrackLayout::fromText(metadata, parseCueTextMetadata(cue)));
      } catch (const std::invalid_argument& error) {
         rejected += std::string("text: ") + error.what() + "; ";
      }
   }
   if (candidates.empty()) {
      throw std::invalid_argument("auto: no valid CUESHEET layout; " + rejected);
   }
   // Compare every pair: unknown native fields cannot mask conflicts between
   // two textual candidates that supply different known values.
   for (std::size_t first = 0; first < candidates.size(); ++first) {
      for (std::size_t second = first + 1; second < candidates.size(); ++second) {
         const auto differences = compareTrackLayouts(candidates[first], candidates[second]);
         if (!differences.empty()) {
            std::string message = "auto layout conflict (use --layout native or text to select explicitly): ";
            for (const auto& difference : differences) {
               message += difference + "; ";
            }
            throw std::invalid_argument(message);
         }
      }
   }
   auto result = candidates.front();
   const auto fillText = [](std::string& destination, const std::string& value) {
      if (!destination.empty() && !value.empty() && destination != value) {
         throw std::invalid_argument("auto: conflicting descriptive CUE metadata; select a layout explicitly");
      }
      if (destination.empty()) {
         destination = value;
      }
   };
   for (std::size_t position = nativeCount; position < candidates.size(); ++position) {
      const auto& text = candidates[position];
      fillText(result.title, text.title);
      fillText(result.performer, text.performer);
      if (result.catalogNumber.empty()) {
         result.catalogNumber = text.catalogNumber;
      }
      for (std::size_t track = 0; track < result.tracks.size(); ++track) {
         auto& target = result.tracks[track];
         const auto& origin = text.tracks[track];
         fillText(target.title, origin.title);
         fillText(target.performer, origin.performer);
         if (target.isrc.empty()) {
            target.isrc = origin.isrc;
         }
         if (!target.flags.digitalCopyPermitted) {
            target.flags.digitalCopyPermitted = origin.flags.digitalCopyPermitted;
         }
      }
   }
   return result;
}


}
