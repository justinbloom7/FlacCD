#include "flaccd/AudioExporter.h"
#include "ExportOutput.h"

#include <algorithm>
#include <limits>
#include <iterator>
#include <set>
#include <stdexcept>

namespace flaccd {

SectorRange getDiscRange(const TrackLayout& layout) {
   if (layout.getTracks().empty()) {
      throw std::invalid_argument("disc layout has no tracks");
   }
   return {layout.getTracks().front().startLba, layout.getLeadOut().discLba};
}

std::vector<SectorRange> getTrackRanges(const TrackLayout& layout,
                                        std::uint32_t firstTrack,
                                        std::uint32_t lastTrack,
                                        bool indexOneOnly) {
   if (firstTrack == 0 || lastTrack < firstTrack || lastTrack > 99) {
      throw std::invalid_argument("track range must increase within 1..99");
   }
   std::vector<SectorRange> result;
   const auto tracks = layout.getTracks();
   for (auto number = firstTrack; number <= lastTrack; ++number) {
      const auto track = std::find_if(tracks.begin(), tracks.end(), [number](const auto& candidate) {
         return candidate.number == number;
      });
      if (track == tracks.end()) {
         throw std::invalid_argument("track " + std::to_string(number) + " does not exist");
      }
      auto begin = track->startLba;
      if (indexOneOnly) {
         const auto index = std::find_if(track->indices.begin(), track->indices.end(), [](const auto& candidate) {
            return candidate.number == 1;
         });
         if (index == track->indices.end()) {
            throw std::invalid_argument("selected track has no INDEX 01");
         }
         begin = index->discLba;
      }
      const auto next = std::next(track);
      const auto end = next == tracks.end() ? layout.getLeadOut().discLba : next->startLba;
      result.push_back({begin, end});
   }
   validateSectorRanges(layout, result);
   return result;
}

std::uint64_t validateSectorRanges(const TrackLayout& layout, std::span<const SectorRange> ranges) {
   if (ranges.empty()) {
      throw std::invalid_argument("no sector ranges selected");
   }
   const auto disc = getDiscRange(layout);
   std::uint64_t total = 0;
   for (const auto& range : ranges) {
      if (range.beginLba < disc.beginLba || range.endLba > disc.endLba || range.beginLba >= range.endLba) {
         throw std::out_of_range("sector range is empty or outside the disc (lead-out is exclusive)");
      }
      const auto count = static_cast<std::uint64_t>(range.endLba) - static_cast<std::uint64_t>(range.beginLba);
      if (count > std::numeric_limits<std::uint64_t>::max() / BYTES_PER_SECTOR - total) {
         throw std::out_of_range("export byte count overflows");
      }
      total += count;
      for (const auto& region : layout.getRegions()) {
         if (region.beginLba < range.endLba && region.endLba > range.beginLba &&
             (region.kind != RegionKind::SourceAudio || !region.sourceStartSector)) {
            throw std::invalid_argument("selected range contains source-less PREGAP/POSTGAP audio");
         }
      }
   }
   return total;
}

void exportRaw(const std::filesystem::path& inputPath,
               const TrackLayout& layout,
               std::span<const RawExport> outputs,
               bool overwrite,
               const ExportProgress& progress) {
   if (outputs.empty()) {
      throw std::invalid_argument("no export outputs selected");
   }
   std::set<std::filesystem::path> destinations;
   std::uint64_t total = 0;
   for (const auto& output : outputs) {
      const auto count = validateSectorRanges(layout, output.ranges);
      if (count > std::numeric_limits<std::uint64_t>::max() / BYTES_PER_SECTOR - total) {
         throw std::out_of_range("total export byte count overflows");
      }
      total += count;
      const auto path = std::filesystem::weakly_canonical(output.outputPath);
      if (!destinations.insert(path).second) {
         throw std::invalid_argument("duplicate export output path");
      }
   }
   CddaSectorReader reader(inputPath, layout);
   std::vector<std::unique_ptr<ExportOutput>> staged;
   for (const auto& output : outputs) {
      staged.push_back(std::make_unique<ExportOutput>(output.outputPath, inputPath, overwrite));
   }
   std::uint64_t completed = 0;
   for (std::size_t position = 0; position < outputs.size(); ++position) {
      writeCddaRanges(reader, *staged[position], outputs[position].ranges, completed, total, progress);
      staged[position]->close();
   }
   for (const auto& output : staged) {
      output->publish();
   }
}

}
