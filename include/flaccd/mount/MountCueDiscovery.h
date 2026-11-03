#pragma once

#include "flaccd/TrackLayout.h"

#include <filesystem>

namespace flaccd {

TrackLayout externalMountLayout(const FlacMetadata& metadata,
                                const CueTextMetadata& cue,
                                const std::filesystem::path& cuePath,
                                const std::filesystem::path& source);
TrackLayout discoverMountLayout(const FlacMetadata& metadata,
                                const std::filesystem::path& source,
                                std::string_view selection);

}
