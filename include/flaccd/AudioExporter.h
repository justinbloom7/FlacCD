#pragma once

#include "flaccd/CddaSectorReader.h"

#include <filesystem>
#include <functional>
#include <span>
#include <vector>

namespace flaccd {

struct SectorRange {
   std::int64_t beginLba = 0;
   std::int64_t endLba = 0; // Exclusive.
};

struct RawExport {
   std::filesystem::path outputPath;
   std::vector<SectorRange> ranges;
};

using ExportProgress = std::function<void(std::uint64_t completed, std::uint64_t total)>;

SectorRange getDiscRange(const TrackLayout& layout);
std::vector<SectorRange> getTrackRanges(const TrackLayout& layout,
                                        std::uint32_t firstTrack,
                                        std::uint32_t lastTrack,
                                        bool indexOneOnly = false);
std::uint64_t validateSectorRanges(const TrackLayout& layout, std::span<const SectorRange> ranges);

// Preflight every destination before decoding. Stage outputs beside their final
// paths and publish only complete files. No-clobber publication is atomic per
// file; multiple outputs are not one atomic filesystem transaction.
void exportRaw(const std::filesystem::path& inputPath,
               const TrackLayout& layout,
               std::span<const RawExport> outputs,
               bool overwrite = false,
               const ExportProgress& progress = {});

}
