#include "flaccd/AudioExporter.h"
#include "flaccd/BinCueExporter.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

void require(bool condition, std::string_view message) {
   if (!condition) {
      throw std::runtime_error(std::string(message));
   }
}

template<typename Operation>
void expectError(Operation operation, std::string_view expectedMessage = {}) {
   try {
      operation();
   } catch (const std::exception& error) {
      require(std::string_view(error.what()).find(expectedMessage) != std::string_view::npos,
              "Error did not contain the expected diagnostic");
      return;
   }
   throw std::runtime_error("Expected export rejection");
}

std::string readFile(const std::filesystem::path& path) {
   std::ifstream input(path, std::ios::binary);
   require(static_cast<bool>(input), "Open test output");
   return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

class TestDirectory {
public:
   explicit TestDirectory(const std::filesystem::path& parent) {
      std::random_device generator;
      for (int attempt = 0; attempt < 100; ++attempt) {
         path = parent / ("audio-export-test-" + std::to_string(generator()));
         if (std::filesystem::create_directory(path)) {
            return;
         }
      }
      throw std::runtime_error("Cannot create test directory");
   }

   ~TestDirectory() {
      std::error_code error;
      std::filesystem::remove_all(path, error);
   }

   std::filesystem::path path;
};

void testSelection(const flaccd::FlacMetadata& metadata) {
   const auto native = flaccd::selectTrackLayout(metadata, "native");
   const auto text = flaccd::selectTrackLayout(metadata, "text");
   const auto automatic = flaccd::selectTrackLayout(metadata, "auto");
   require(automatic.getLeadOut().fromNativeMetadata, "Auto did not prefer native geometry");
   require(flaccd::compareTrackLayouts(native, automatic).empty() &&
           flaccd::compareTrackLayouts(text, automatic).empty(), "Auto changed shared layout fields");
   require(automatic.getTracks()[0].flags.digitalCopyPermitted.value_or(false), "Auto lost known DCP");

   auto modified = metadata;
   modified.embeddedCueSheets[0] = "TITLE \"Album\"\nPERFORMER \"Artist\"\n" + modified.embeddedCueSheets[0];
   auto cuePosition = modified.embeddedCueSheets[0].find("TRACK 01 AUDIO\n");
   modified.embeddedCueSheets[0].insert(cuePosition + std::string("TRACK 01 AUDIO\n").size(), "TITLE \"Song\"\n");
   const auto described = flaccd::selectTrackLayout(modified, "auto");
   require(described.getLeadOut().fromNativeMetadata && described.getTitle() == "Album" &&
           described.getPerformer() == "Artist" && described.getTracks()[0].title == "Song",
           "Auto lost compatible text descriptions");

   modified = metadata;
   modified.embeddedCueSheets[0].replace(modified.embeddedCueSheets[0].find("00:00:28"), 8, "00:00:29");
   expectError([&] { flaccd::selectTrackLayout(modified, "auto"); }, "conflict");
   require(flaccd::selectTrackLayout(modified, "native").getLeadOut().fromNativeMetadata,
           "Explicit native selection rejected an unrelated text conflict");
   modified = metadata;
   modified.cueSheets[0].tracks.back().offset -= flaccd::SAMPLES_PER_SECTOR;
   require(!flaccd::selectTrackLayout(modified, "auto").getLeadOut().fromNativeMetadata,
           "Auto did not fall back to valid text");
   modified = metadata;
   modified.embeddedCueSheets[0] = "Malformed CUE\n";
   require(flaccd::selectTrackLayout(modified, "auto").getLeadOut().fromNativeMetadata,
           "Auto did not retain valid native when text was invalid");
   modified.cueSheets.clear();
   expectError([&] { flaccd::selectTrackLayout(modified, "auto"); }, "no valid");
   modified.embeddedCueSheets.clear();
   expectError([&] { flaccd::selectTrackLayout(modified, "auto"); }, "no valid");

   modified = metadata;
   auto secondText = metadata.embeddedCueSheets[0];
   secondText.replace(secondText.find("FLAGS PRE DCP"), 13, "FLAGS PRE");
   modified.embeddedCueSheets.push_back(secondText);
   expectError([&] { flaccd::selectTrackLayout(modified, "auto"); }, "conflict");
}

void testRanges(const flaccd::FlacMetadata& metadata) {
   const auto layout = flaccd::selectTrackLayout(metadata, "auto");
   const auto disc = flaccd::getDiscRange(layout);
   require(disc.beginLba == -2 && disc.endLba == 238, "Disc range dropped first-track pregap");
   const auto ranges = flaccd::getTrackRanges(layout, 1, 3);
   const auto trimmed = flaccd::getTrackRanges(layout, 1, 3, true);
   require(ranges.size() == 3 && ranges[0].beginLba == -2 && ranges[0].endLba == 23 &&
           ranges[1].beginLba == 23 && ranges[1].endLba == 58 && ranges[2].endLba == 238,
           "Track slices contain another track's pregap");
   require(trimmed[0].beginLba == 0 && trimmed[0].endLba == 23 && trimmed[1].beginLba == 26 &&
           trimmed[1].endLba == 58 && trimmed[2].beginLba == 58, "INDEX 01-only track boundaries");
   require(flaccd::validateSectorRanges(layout, ranges) == 240 &&
           flaccd::validateSectorRanges(layout, trimmed) == 235, "Track sector counts differ");
   expectError([&] { flaccd::getTrackRanges(layout, 0, 1); });
   expectError([&] { flaccd::getTrackRanges(layout, 3, 2); });
   expectError([&] { flaccd::getTrackRanges(layout, 1, 4); }, "does not exist");
   for (const auto range : {flaccd::SectorRange{-3, 0}, flaccd::SectorRange{237, 239},
                            flaccd::SectorRange{238, 238},
                            flaccd::SectorRange{std::numeric_limits<std::int64_t>::min(), 1},
                            flaccd::SectorRange{0, std::numeric_limits<std::int64_t>::max()}}) {
      expectError([&] { flaccd::validateSectorRanges(layout, std::span(&range, 1)); });
   }
   auto nativeCue = metadata.cueSheets[0];
   nativeCue.tracks[0].number = 2;
   nativeCue.tracks[1].number = 4;
   nativeCue.tracks[2].number = 6;
   const auto numbered = flaccd::TrackLayout::fromNative(metadata, nativeCue);
   require(flaccd::getTrackRanges(numbered, 2, 2)[0].beginLba == -2, "Used zero-based track indexes");
   require(flaccd::getTrackRanges(numbered, 4, 4)[0].beginLba == 23, "Used track number as array index");
   expectError([&] { flaccd::getTrackRanges(numbered, 1, 1); }, "does not exist");
   expectError([&] { flaccd::getTrackRanges(numbered, 2, 4); }, "does not exist");
}

void testCueTimestamps() {
   flaccd::FlacMetadata metadata;
   metadata.sampleRate = 44100;
   metadata.channels = 2;
   metadata.bitsPerSample = 16;
   metadata.totalSamples = 5000 * flaccd::SAMPLES_PER_SECTOR;
   const auto text = flaccd::parseCueTextMetadata("TITLE \"Clock\"\nFILE source.flac FLAC\n"
                                                 "TRACK 01 AUDIO\nINDEX 00 00:00:00\nINDEX 01 00:00:02\n"
                                                 "TRACK 02 AUDIO\nINDEX 00 00:59:74\nINDEX 01 01:00:01\n");
   const auto original = flaccd::TrackLayout::fromText(metadata, text);
   const auto cue = flaccd::generateCueSheet(original, metadata, "Special & [spaces] é.bin");
   require(cue.find("INDEX 00 00:59:74") != std::string::npos &&
           cue.find("INDEX 01 01:00:01") != std::string::npos, "Minute rollover changed exact sector timestamps");
   const auto result = flaccd::TrackLayout::fromBinaryCue(5000 * flaccd::BYTES_PER_SECTOR,
                                                        flaccd::parseCueTextMetadata(cue));
   require(flaccd::compareTrackLayouts(original, result).empty(), "Minute boundary CUE round trip");
}

void testStaging(const std::filesystem::path& input,
                 const flaccd::FlacMetadata& metadata,
                 const std::filesystem::path& directory) {
   const auto layout = flaccd::selectTrackLayout(metadata, "auto");
   const auto finalPath = directory / "interrupted.raw";
   const flaccd::RawExport output{finalPath, {flaccd::getDiscRange(layout)}};
   bool observed = false;
   expectError([&] {
      flaccd::exportRaw(input, layout, std::span(&output, 1), false, [&](auto completed, auto total) {
         require(completed > 0 && completed < total, "Interruption should occur before completion");
         require(!std::filesystem::exists(finalPath), "Partial audio used final filename");
         bool foundPart = false;
         for (const auto& entry : std::filesystem::directory_iterator(directory)) {
            foundPart = foundPart || entry.path().extension() == ".part";
         }
         require(foundPart, "No recognizable staging path during export");
         observed = true;
         throw std::runtime_error("simulated interruption");
      });
   }, "simulated interruption");
   require(observed && !std::filesystem::exists(finalPath), "Interrupted export left final-looking output");
   require(std::filesystem::is_empty(directory), "Handled interruption left staging files");
   {
      std::ofstream existing(finalPath);
      existing << "Keep existing data";
   }
   expectError([&] {
      flaccd::exportRaw(input, layout, std::span(&output, 1), true, [](auto, auto) {
         throw std::runtime_error("simulated interruption");
      });
   }, "simulated interruption");
   require(readFile(finalPath) == "Keep existing data", "Interrupted overwrite changed old output");
   flaccd::exportRaw(input, layout, std::span(&output, 1), true);
   require(readFile(finalPath) == readFile(input.parent_path() / "original.raw"), "Completed overwrite differs from PCM");
}

}

int main(int argumentCount, char* arguments[]) {
   try {
      require(argumentCount == 2, "Expected fixture directory");
      const auto fixtureDirectory = std::filesystem::absolute(arguments[1]);
      const auto input = fixtureDirectory / "fixture-4096.flac";
      const auto metadata = flaccd::FlacMetadataInspector{}.inspect(input);
      TestDirectory directory(fixtureDirectory);
      testSelection(metadata);
      testRanges(metadata);
      testCueTimestamps();
      testStaging(input, metadata, directory.path);
      std::cout << "Audio exporter tests passed\n";
   } catch (const std::exception& error) {
      std::cerr << error.what() << '\n';
      return 1;
   }
   return 0;
}
