#pragma once

#include "flaccd/CueTextMetadata.h"
#include "flaccd/FlacMetadataInspector.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace flaccd {

constexpr std::uint64_t SAMPLES_PER_SECTOR = 588;
constexpr std::uint64_t SECTORS_PER_SECOND = 75;

enum class TrackType { Audio };
enum class RegionKind { SourceAudio, Pregap, Postgap };
enum class IndexOrigin { Metadata, PregapDirective };

struct TrackFlags {
   bool preEmphasis = false;
   // Native FLAC CUESHEET has no DCP field; unknown is distinct from false.
   std::optional<bool> digitalCopyPermitted;
};

struct LayoutIndex {
   std::uint32_t number = 0;
   std::int64_t discLba = 0;
   std::optional<std::uint64_t> sourceSector;
   IndexOrigin origin = IndexOrigin::Metadata;
};

struct LayoutTrack {
   std::uint32_t number = 0;
   TrackType type = TrackType::Audio;
   TrackFlags flags;
   std::string isrc;
   std::string title;
   std::string performer;
   std::vector<LayoutIndex> indices;
   std::uint64_t storedPregapSectors = 0;
   std::optional<std::uint64_t> declaredPregapSectors;
   std::optional<std::uint64_t> declaredPostgapSectors;
   std::int64_t startLba = 0;
   std::int64_t endLba = 0; // Exclusive, including this track's postgap.
};

struct LayoutRegion {
   std::int64_t beginLba = 0;
   std::int64_t endLba = 0; // Exclusive.
   std::uint32_t trackNumber = 0;
   std::optional<std::uint32_t> indexNumber; // No defined index for POSTGAP.
   RegionKind kind = RegionKind::SourceAudio;
   std::optional<std::uint64_t> sourceStartSector;
};

struct LeadOut {
   std::int64_t discLba = 0;
   std::uint64_t sourceSector = 0;
   bool fromNativeMetadata = false; // Text sheets use the exact STREAMINFO end.
};

struct DiscPosition {
   std::uint32_t trackNumber = 0;
   std::optional<std::uint32_t> indexNumber;
   RegionKind kind = RegionKind::SourceAudio;
   std::optional<std::uint64_t> sourceSector;
};

class TrackLayout {
public:
   // Require CD-DA-compatible STREAMINFO and an exact, known source length.
   // Throw std::invalid_argument on malformed or unsupported layout metadata.
   static TrackLayout fromNative(const FlacMetadata& metadata, const NativeCueSheet& cueSheet);
   static TrackLayout fromText(const FlacMetadata& metadata, const CueTextMetadata& cueSheet);

   // Parse an external single-file BINARY audio CUE using the BIN byte length.
   // This validates metadata only and does not open the referenced file.
   static TrackLayout fromBinaryCue(std::uint64_t byteCount, const CueTextMetadata& cueSheet);

   std::span<const LayoutTrack> getTracks() const;
   const std::string& getTitle() const;
   const std::string& getPerformer() const;
   std::span<const LayoutRegion> getRegions() const;
   const LeadOut& getLeadOut() const;
   const std::optional<std::uint64_t>& getLeadInSamples() const;
   const std::string& getCatalogNumber() const;

   // LBA 0 is first track INDEX 01. Explicit first-track gaps may have negative
   // LBAs. No implicit 150-sector gap is added. Lead-out and outside return null.
   std::optional<DiscPosition> queryLba(std::int64_t discLba) const;

private:
   friend TrackLayout selectTrackLayout(const FlacMetadata& metadata, std::string_view source);
   TrackLayout() = default;
   static TrackLayout normalize(const FlacMetadata& metadata, std::vector<LayoutTrack> tracks);

   std::vector<LayoutTrack> tracks;
   std::vector<LayoutRegion> regions;
   LeadOut leadOut;
   std::optional<std::uint64_t> leadInSamples;
   std::string catalogNumber;
   std::string title;
   std::string performer;
};

// Compares shared structural information only. Unknown DCP/ISRC/catalog values
// are not inferred. Descriptive titles and native-only lead-in are not merged.
std::vector<std::string> compareTrackLayouts(const TrackLayout& first, const TrackLayout& second);

// Explicit selection never merges sources. Auto compares all valid layouts,
// prefers native geometry, and retains compatible text metadata absent natively.
TrackLayout selectTrackLayout(const FlacMetadata& metadata, std::string_view source);

}
