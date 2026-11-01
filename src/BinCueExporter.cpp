#include "flaccd/BinCueExporter.h"
#include "ExportOutput.h"

#include <stdexcept>

namespace flaccd {

void exportBinCue(const std::filesystem::path& inputPath,
                  const TrackLayout& layout,
                  const std::filesystem::path& cuePath,
                  bool overwrite,
                  const ExportProgress& progress) {
   if (cuePath.extension() != ".cue") {
      throw std::invalid_argument("output path must end in .cue");
   }
   auto binPath = cuePath;
   binPath.replace_extension(".bin");
   const auto metadata = FlacMetadataInspector{}.inspect(inputPath);
   const auto cue = generateCueSheet(layout, metadata, binPath.filename().string());
   CddaSectorReader reader(inputPath, layout);
   ExportOutput bin(binPath, inputPath, overwrite);
   ExportOutput cueFile(cuePath, inputPath, overwrite);
   const auto range = getDiscRange(layout);
   const auto total = validateSectorRanges(layout, std::span(&range, 1));
   std::uint64_t completed = 0;
   writeCddaRanges(reader, bin, std::span(&range, 1), completed, total, progress);
   cueFile.write(cue.data(), cue.size());
   bin.close();
   cueFile.close();
   // Publish the CUE last: it never points at a partially decoded new BIN.
   bin.publish();
   cueFile.publish();
}

void exportCueSheet(const std::filesystem::path& inputPath,
                    const TrackLayout& layout,
                    const std::filesystem::path& cuePath,
                    const std::string& binName,
                    bool overwrite) {
   if (cuePath.extension() != ".cue") {
      throw std::invalid_argument("output path must end in .cue");
   }
   if (getDiscRange(layout).beginLba != 0) {
      throw std::invalid_argument("CUE-only requires complete source audio starting at disc LBA 0; "
                                  "use full BIN/CUE for first-track pregaps");
   }
   const auto metadata = FlacMetadataInspector{}.inspect(inputPath);
   const auto cue = generateCueSheet(layout, metadata, binName);
   const auto binPath = cuePath.parent_path() / binName;
   if (std::filesystem::absolute(binPath).lexically_normal() ==
       std::filesystem::absolute(cuePath).lexically_normal() ||
       (std::filesystem::exists(binPath) && std::filesystem::exists(cuePath) &&
        std::filesystem::equivalent(binPath, cuePath))) {
      throw std::invalid_argument("CUE output must not alias the referenced BIN");
   }
   if (std::filesystem::exists(binPath) &&
       (!std::filesystem::is_regular_file(binPath) ||
        std::filesystem::file_size(binPath) != layout.getLeadOut().sourceSector * BYTES_PER_SECTOR)) {
      throw std::invalid_argument("referenced BIN has an incompatible size or is not a regular file");
   }
   ExportOutput output(cuePath, inputPath, overwrite);
   output.write(cue.data(), cue.size());
   output.close();
   output.publish();
}

}
