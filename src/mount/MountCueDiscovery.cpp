#include "flaccd/mount/MountCueDiscovery.h"
#include "flaccd/mount/MountFileSystem.h"
#include "flaccd/PathText.h"

#include <stdexcept>

namespace flaccd {

TrackLayout externalMountLayout(const FlacMetadata& metadata,
                                const CueTextMetadata& cue,
                                const std::filesystem::path& cuePath,
                                const std::filesystem::path& source) {
   auto layout = TrackLayout::fromText(metadata, cue);
   const auto file = pathFromUtf8(cue.fileName);
   const auto reference = file.is_absolute() ? file : cuePath.parent_path() / file;
   std::error_code error;
   if (!std::filesystem::equivalent(reference, source, error) || error) {
      throw std::invalid_argument("External CUE " + pathToUtf8(cuePath) +
                                  " does not refer to the selected FLAC: " + pathToUtf8(source));
   }
   return layout;
}

TrackLayout discoverMountLayout(const FlacMetadata& metadata,
                                const std::filesystem::path& source,
                                std::string_view selection) {
   // Presence, rather than validity, controls fallback: never hide an embedded
   // conflict or malformed sheet by substituting an external one.
   if (!metadata.cueSheets.empty() || !metadata.embeddedCueSheets.empty() || selection == "native") {
      return selectTrackLayout(metadata, selection);
   }
   if (selection != "auto" && selection != "text") {
      return selectTrackLayout(metadata, selection);
   }
   auto cuePath = source;
   cuePath.replace_extension(".cue");
   if (!mountPathExists(cuePath)) {
      throw std::invalid_argument("No embedded CUE sheet and " + pathToUtf8(cuePath) +
                                  " does not exist. A cue sheet is required.\nFLAC: " + pathToUtf8(source));
   }
   return externalMountLayout(metadata, parseCueTextMetadata(readMountCue(cuePath)), cuePath, source);
}

}
