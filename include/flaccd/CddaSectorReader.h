#pragma once

#include "flaccd/TrackLayout.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>

namespace flaccd {

constexpr std::size_t BYTES_PER_SECTOR = 2352;
using CddaSector = std::array<std::byte, BYTES_PER_SECTOR>;

class CddaSectorReader {
public:
   // The caller supplies the layout belonging to this file. Format and source
   // length are checked; equal-length files cannot be distinguished by layout.
   CddaSectorReader(const std::filesystem::path& path, const TrackLayout& layout);
   ~CddaSectorReader();
   CddaSectorReader(CddaSectorReader&& other) noexcept;
   CddaSectorReader& operator=(CddaSectorReader&& other) noexcept;
   CddaSectorReader(const CddaSectorReader&) = delete;
   CddaSectorReader& operator=(const CddaSectorReader&) = delete;

   // Returns interleaved signed 16-bit little-endian left/right PCM, unchanged
   // by PRE/DCP. Stored INDEX 00 audio, including negative LBAs, is preserved.
   // Throws out_of_range for invalid LBAs, invalid_argument for source-less
   // gaps, and runtime_error on decoder errors. No partial sector is returned.
   // A decoder error makes this reader unusable; invalid LBAs/gaps do not.
   // Instances are independent, but each instance requires external locking.
   CddaSector readSector(std::int64_t discLba);

private:
   class Impl;
   std::unique_ptr<Impl> impl;
};

}
