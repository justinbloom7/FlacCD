#pragma once

#include "flaccd/AudioExporter.h"

#include <cstdio>
#include <memory>

namespace flaccd {

class ExportOutput {
public:
   ExportOutput(const std::filesystem::path& outputPath,
                const std::filesystem::path& inputPath,
                bool overwrite);
   ~ExportOutput();
   ExportOutput(const ExportOutput&) = delete;
   ExportOutput& operator=(const ExportOutput&) = delete;

   void write(const void* data, std::size_t size);
   void close();
   void publish();

private:
   std::filesystem::path finalPath;
   std::filesystem::path stagingDirectory;
   std::filesystem::path stagingPath;
   std::unique_ptr<std::FILE, decltype(&std::fclose)> file{nullptr, &std::fclose};
   bool overwrite = false;
};

void writeCddaRanges(CddaSectorReader& reader,
                     ExportOutput& output,
                     std::span<const SectorRange> ranges,
                     std::uint64_t& completed,
                     std::uint64_t total,
                     const ExportProgress& progress);

}
