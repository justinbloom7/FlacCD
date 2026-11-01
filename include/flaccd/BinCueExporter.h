#pragma once

#include "flaccd/AudioExporter.h"
#include "flaccd/CueSheetWriter.h"

#include <filesystem>
#include <string>

namespace flaccd {

// cuePath must end in .cue; BIN uses the same stem beside it. Stage both files
// and publish complete BIN first, CUE last. The pair is not crash-atomic.
void exportBinCue(const std::filesystem::path& inputPath,
                  const TrackLayout& layout,
                  const std::filesystem::path& cuePath,
                  bool overwrite = false,
                  const ExportProgress& progress = {});

// CUE-only requires source sector zero at disc LBA 0; negative first-track
// pregaps are rejected. Does not create/read audio. An existing BIN must have the exact
// expected size. A missing BIN is permitted and remains the caller's responsibility.
void exportCueSheet(const std::filesystem::path& inputPath,
                    const TrackLayout& layout,
                    const std::filesystem::path& cuePath,
                    const std::string& binName,
                    bool overwrite = false);

}
