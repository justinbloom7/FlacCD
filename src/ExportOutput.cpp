#include "ExportOutput.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <random>
#include <stdexcept>

namespace flaccd {

ExportOutput::ExportOutput(const std::filesystem::path& outputPath,
                           const std::filesystem::path& inputPath,
                           bool allowOverwrite)
   : finalPath(std::filesystem::absolute(outputPath).lexically_normal()), overwrite(allowOverwrite) {
   const auto status = std::filesystem::symlink_status(finalPath);
   if (std::filesystem::is_symlink(status)) {
      throw std::invalid_argument("refusing output symlink: " + finalPath.string());
   }
   if (std::filesystem::exists(status)) {
      if (std::filesystem::equivalent(finalPath, inputPath)) {
         throw std::invalid_argument("output aliases input audio: " + finalPath.string());
      }
      if (!overwrite || !std::filesystem::is_regular_file(status)) {
         throw std::invalid_argument("output exists or is not a regular file: " + finalPath.string());
      }
   }
   std::random_device generator;
   for (int attempt = 0; attempt < 100; ++attempt) {
      const auto candidate = finalPath.parent_path() / (".flaccd-export-" + std::to_string(generator()) + ".part");
      if (std::filesystem::create_directory(candidate)) {
         stagingDirectory = candidate;
         break;
      }
   }
   if (stagingDirectory.empty()) {
      throw std::runtime_error("cannot create staging directory beside " + finalPath.string());
   }
   stagingPath = stagingDirectory / "output.part";
   file.reset(std::fopen(stagingPath.string().c_str(), "wbx"));
   if (!file) {
      const auto message = std::string(std::strerror(errno));
      std::error_code error;
      std::filesystem::remove_all(stagingDirectory, error);
      throw std::runtime_error("cannot create staged output: " + message);
   }
   try {
      if (!overwrite) {
         const auto probe = stagingDirectory / "link-probe.part";
         std::filesystem::create_hard_link(stagingPath, probe);
         std::filesystem::remove(probe);
      }
   } catch (...) {
      file.reset();
      std::error_code error;
      std::filesystem::remove_all(stagingDirectory, error);
      throw;
   }
}

ExportOutput::~ExportOutput() {
   file.reset();
   std::error_code error;
   std::filesystem::remove_all(stagingDirectory, error);
}

void ExportOutput::write(const void* data, std::size_t size) {
   if (!file || std::fwrite(data, 1, size, file.get()) != size) {
      throw std::runtime_error("failed to write staged output for " + finalPath.string());
   }
}

void ExportOutput::close() {
   if (file && std::fclose(file.release()) != 0) {
      throw std::runtime_error("failed to close staged output for " + finalPath.string());
   }
}

void ExportOutput::publish() {
   if (file) {
      throw std::logic_error("staged output must be closed before publication");
   }
   if (overwrite) {
      // Same-filesystem replacement never truncates an existing inode. On a
      // platform that refuses replacement, report the error and retain it.
      std::filesystem::rename(stagingPath, finalPath);
   } else {
      // Unlike rename(), hard-link creation cannot overwrite a concurrent file.
      std::filesystem::create_hard_link(stagingPath, finalPath);
   }
}

void writeCddaRanges(CddaSectorReader& reader,
                     ExportOutput& output,
                     std::span<const SectorRange> ranges,
                     std::uint64_t& completed,
                     std::uint64_t total,
                     const ExportProgress& progress) {
   constexpr std::size_t BUFFER_SECTORS = 75;
   std::array<std::byte, BUFFER_SECTORS * BYTES_PER_SECTOR> buffer;
   for (const auto& range : ranges) {
      auto lba = range.beginLba;
      while (lba < range.endLba) {
         std::size_t count = 0;
         while (count < BUFFER_SECTORS && lba < range.endLba) {
            const auto sector = reader.readSector(lba);
            std::copy(sector.begin(), sector.end(), buffer.begin() + count * BYTES_PER_SECTOR);
            ++count;
            ++lba;
         }
         output.write(buffer.data(), count * BYTES_PER_SECTOR);
         completed += count;
         if (progress) {
            progress(completed, total);
         }
      }
   }
}

}
