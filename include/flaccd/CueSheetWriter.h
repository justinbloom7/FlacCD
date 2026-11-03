#pragma once

#include "flaccd/TrackLayout.h"

#include <filesystem>
#include <string>

namespace flaccd {

// Complete source-backed audio only. Both writers reparse their output through
// the existing parser and compare normalized layouts before returning.
std::string generateCueSheet(const TrackLayout& layout,
                             const FlacMetadata& metadata,
                             const std::string& binName);
std::string generateFlacCueSheet(const TrackLayout& layout,
                                 const FlacMetadata& metadata,
                                 const std::filesystem::path& sourcePath);

// Relative, filename-only FLAC reference for backend staging. No filesystem
// access is performed; callers resolve/verify the adjacent audio themselves.
std::string generateRelativeFlacCueSheet(const TrackLayout& layout,
                                         const FlacMetadata& metadata,
                                         const std::string& sourceName);

}
