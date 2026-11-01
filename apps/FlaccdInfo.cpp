#include "flaccd/FlacMetadataInspector.h"
#include "flaccd/CueTextMetadata.h"
#include "flaccd/TrackLayout.h"

#include <exception>
#include <iomanip>
#include <iostream>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

void printDescriptiveMetadata(const flaccd::FlacMetadata& metadata,
                              std::span<const flaccd::CueTextMetadata> textCues) {
   std::cout << "Vorbis metadata tags:\n";
   bool foundTags = false;
   for (const auto& comment : metadata.vorbisComments) {
      const auto separator = comment.find('=');
      if (separator == std::string::npos) {
         continue;
      }
      auto key = comment.substr(0, separator);
      for (auto& character : key) {
         if (character >= 'a' && character <= 'z') {
            character = static_cast<char>(character - 'a' + 'A');
         }
      }
      if (key == "ARTIST" || key == "ALBUMARTIST" || key == "ALBUM" ||
          key == "TITLE" || key == "TRACKNUMBER") {
         std::cout << "  " << key << ": " << comment.substr(separator + 1) << '\n';
         foundTags = true;
      }
   }
   if (!foundTags) {
      std::cout << "  (none)\n";
   }
   std::size_t cueNumber = 0;
   for (const auto& descriptive : textCues) {
      std::cout << "CUE descriptive metadata " << ++cueNumber << ":\n"
                << "  Artist: " << (descriptive.performer.empty() ? "(not specified)" : descriptive.performer)
                << "\n  Album: " << (descriptive.title.empty() ? "(not specified)" : descriptive.title) << '\n';
      for (const auto& track : descriptive.tracks) {
         const auto& artist = track.performer.empty() ? descriptive.performer : track.performer;
         std::cout << "  Track " << track.number << " title: "
                   << (track.title.empty() ? "(not specified)" : track.title) << '\n'
                   << "    Artist: " << (artist.empty() ? "(not specified)" : artist) << '\n';
      }
   }
}

void printLayout(const flaccd::TrackLayout& layout) {
   if (layout.getLeadInSamples()) {
      std::cout << "  Native lead-in: " << *layout.getLeadInSamples() << " samples (outside LBA map)\n";
   }
   for (const auto& track : layout.getTracks()) {
      std::cout << "  Layout track " << track.number << " AUDIO: LBA [" << track.startLba
                << ", " << track.endLba << ")\n"
                << "    Stored pregap: " << track.storedPregapSectors << " sectors; PRE: "
                << (track.flags.preEmphasis ? "yes" : "no") << "; DCP: ";
      if (track.flags.digitalCopyPermitted) {
         std::cout << (*track.flags.digitalCopyPermitted ? "yes" : "no");
      } else {
         std::cout << "unknown";
      }
      std::cout << "; ISRC: " << (track.isrc.empty() ? "(not specified)" : track.isrc) << '\n';
      if (track.declaredPregapSectors) {
         std::cout << "    PREGAP: " << *track.declaredPregapSectors << " sectors (no source audio)\n";
      }
      if (track.declaredPostgapSectors) {
         std::cout << "    POSTGAP: " << *track.declaredPostgapSectors << " sectors (no source audio)\n";
      }
      for (const auto& index : track.indices) {
         std::cout << "    Layout INDEX " << index.number << ": LBA " << index.discLba << "; source sector: ";
         if (index.sourceSector) {
            std::cout << *index.sourceSector;
         } else {
            std::cout << "none (PREGAP directive)";
         }
         std::cout << '\n';
      }
   }
   const auto& leadOut = layout.getLeadOut();
   std::cout << "  Lead-out LBA: " << leadOut.discLba << "; source end sector: " << leadOut.sourceSector
             << (leadOut.fromNativeMetadata ? " (native metadata)" : " (from STREAMINFO)") << '\n';
   for (const auto& region : layout.getRegions()) {
      if (region.kind != flaccd::RegionKind::SourceAudio) {
         std::cout << "  No-source region: LBA [" << region.beginLba << ", " << region.endLba
                   << "), track " << region.trackNumber
                   << (region.kind == flaccd::RegionKind::Pregap ? " PREGAP (index 0)" : " POSTGAP (index unspecified)")
                   << '\n';
      }
   }
}

void printNormalizedLayouts(const flaccd::FlacMetadata& metadata,
                            std::span<const flaccd::CueTextMetadata> textCues) {
   std::cout << "Normalized layouts (first track INDEX 01 = LBA 0; sectors are 1/75 second):\n";
   if (metadata.cueSheets.empty() && textCues.empty()) {
      std::cout << "  Unavailable: no CUESHEET metadata\n";
      return;
   }
   std::vector<std::pair<std::string, flaccd::TrackLayout>> candidates;
   for (std::size_t position = 0; position < metadata.cueSheets.size(); ++position) {
      const auto label = "native CUESHEET " + std::to_string(position + 1);
      std::cout << "Normalized layout: " << label << '\n';
      try {
         auto layout = flaccd::TrackLayout::fromNative(metadata, metadata.cueSheets[position]);
         printLayout(layout);
         candidates.emplace_back(label, std::move(layout));
      } catch (const std::invalid_argument& error) {
         std::cout << "  REJECTED layout: " << error.what() << '\n';
      }
   }
   for (std::size_t position = 0; position < textCues.size(); ++position) {
      const auto label = "text CUESHEET " + std::to_string(position + 1);
      std::cout << "Normalized layout: " << label << '\n';
      if (!textCues[position].errors.empty()) {
         for (const auto& error : textCues[position].errors) {
            std::cout << "  REJECTED layout: " << error << '\n';
         }
         continue;
      }
      try {
         auto layout = flaccd::TrackLayout::fromText(metadata, textCues[position]);
         printLayout(layout);
         candidates.emplace_back(label, std::move(layout));
      } catch (const std::invalid_argument& error) {
         std::cout << "  REJECTED layout: " << error.what() << '\n';
      }
   }
   for (std::size_t first = 0; first < candidates.size(); ++first) {
      for (std::size_t second = first + 1; second < candidates.size(); ++second) {
         const auto differences = flaccd::compareTrackLayouts(candidates[first].second, candidates[second].second);
         std::cout << "Layout comparison: " << candidates[first].first << " vs " << candidates[second].first << '\n';
         if (differences.empty()) {
            std::cout << "  Shared layout fields agree; unknown fields remain unknown\n";
         } else {
            for (const auto& difference : differences) {
               std::cout << "  CONFLICT: " << difference << '\n';
            }
         }
      }
   }
}

void printMetadata(const flaccd::FlacMetadata& metadata) {
   std::vector<flaccd::CueTextMetadata> textCues;
   for (const auto& cueText : metadata.embeddedCueSheets) {
      textCues.push_back(flaccd::parseCueTextMetadata(cueText));
   }
   printDescriptiveMetadata(metadata, textCues);
   printNormalizedLayouts(metadata, textCues);
   std::cout << "Sample rate: " << metadata.sampleRate << " Hz\n"
             << "Channels: " << metadata.channels << '\n'
             << "Bit depth: " << metadata.bitsPerSample << " bits\n"
             << "Total sample count: " << metadata.totalSamples;
   if (metadata.totalSamples == 0) {
      std::cout << " (unknown)\nDuration: unknown\n";
   } else {
      // Floating point is used only for human-readable duration, never for offsets.
      std::cout << "\nDuration: " << std::fixed << std::setprecision(6)
                << static_cast<double>(metadata.totalSamples) / metadata.sampleRate << " seconds\n";
   }
   const bool isCdFormat = metadata.sampleRate == 44100 && metadata.channels == 2 &&
                           metadata.bitsPerSample == 16;
   std::cout << "CD-DA audio format: " << (isCdFormat ? "yes" : "no") << '\n';
   std::cout << "Native CUESHEET blocks: " << metadata.cueSheets.size() << '\n';
   for (const auto& cueSheet : metadata.cueSheets) {
      std::cout << "  Catalog: " << cueSheet.catalogNumber << '\n'
                << "  Lead-in: " << cueSheet.leadIn << " samples\n"
                << "  CD flag: " << (cueSheet.isCd ? "yes" : "no") << '\n';
      for (const auto& track : cueSheet.tracks) {
         const bool isLeadOut = track.number == (cueSheet.isCd ? 170 : 255);
         std::cout << "  Track " << static_cast<unsigned int>(track.number)
                   << ": offset " << track.offset << " samples (stream-relative)"
                   << (isLeadOut ? " [lead-out]" : "") << '\n'
                   << "    Type: " << (track.isAudio ? "audio" : "data")
                   << "; PRE: " << (track.preEmphasis ? "yes" : "no")
                   << "; ISRC: " << track.isrc << '\n';
         for (const auto& index : track.indices) {
            std::cout << "    INDEX " << static_cast<unsigned int>(index.number)
                      << ": offset " << index.offset << " samples (track-relative); stream offset: ";
            if (index.offset > std::numeric_limits<std::uint64_t>::max() - track.offset) {
               std::cout << "overflow\n";
            } else {
               std::cout << track.offset + index.offset << " samples\n";
            }
         }
      }
   }
   std::cout << "Embedded CUESHEET comments: " << metadata.embeddedCueSheets.size() << '\n';
   for (const auto& cueText : metadata.embeddedCueSheets) {
      std::cout << "--- CUESHEET (verbatim) ---\n" << cueText;
      if (cueText.empty() || cueText.back() != '\n') {
         std::cout << '\n';
      }
      std::cout << "--- end CUESHEET ---\n";
   }
}

}

int main(int argumentCount, char* arguments[]) {
   if (argumentCount == 2 && std::string_view(arguments[1]) == "--help") {
      std::cout << "Usage: flaccd-info <file.flac>\nInspect native FLAC metadata without decoding audio.\n";
      return 0;
   }
   if (argumentCount != 2) {
      std::cerr << "Usage: flaccd-info <file.flac>\n";
      return 2;
   }
   try {
      printMetadata(flaccd::FlacMetadataInspector{}.inspect(arguments[1]));
   } catch (const std::exception& error) {
      std::cerr << "flaccd-info: " << arguments[1] << ": " << error.what() << '\n';
      return 1;
   }
   return 0;
}
