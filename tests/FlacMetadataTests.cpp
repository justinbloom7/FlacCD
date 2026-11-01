#include "flaccd/FlacMetadataInspector.h"
#include "flaccd/CueTextMetadata.h"
#include "flaccd/TrackLayout.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using Bytes = std::vector<std::uint8_t>;
constexpr std::string_view CUE_TEXT = "FILE \"disc.flac\" WAVE\n  TRACK 01 AUDIO\n    FLAGS PRE DCP\n    INDEX 00 00:00:00\n    INDEX 01 00:00:01\n";

void require(bool condition, std::string_view message) {
   if (!condition) {
      throw std::runtime_error(std::string(message));
   }
}

void appendBigEndian(Bytes& bytes, std::uint64_t value, unsigned int byteCount) {
   for (unsigned int index = byteCount; index > 0; --index) {
      bytes.push_back(static_cast<std::uint8_t>(value >> ((index - 1) * 8)));
   }
}

void appendLittleEndian(Bytes& bytes, std::uint32_t value) {
   for (unsigned int index = 0; index < 4; ++index) {
      bytes.push_back(static_cast<std::uint8_t>(value >> (index * 8)));
   }
}

void appendString(Bytes& bytes, std::string_view text) {
   bytes.insert(bytes.end(), text.begin(), text.end());
}

void appendBlock(Bytes& file, std::uint8_t type, const Bytes& body, bool isLast) {
   file.push_back(static_cast<std::uint8_t>(type | (isLast ? 0x80 : 0)));
   appendBigEndian(file, body.size(), 3);
   file.insert(file.end(), body.begin(), body.end());
}

Bytes makeStreamInfo(std::uint32_t sampleRate, std::uint32_t channels,
                     std::uint32_t bitsPerSample, std::uint64_t totalSamples) {
   Bytes body;
   appendBigEndian(body, 4096, 2);
   appendBigEndian(body, 4096, 2);
   body.resize(10, 0); // Unknown minimum/maximum frame sizes.
   const std::uint64_t packed = (static_cast<std::uint64_t>(sampleRate) << 44) |
                                (static_cast<std::uint64_t>(channels - 1) << 41) |
                                (static_cast<std::uint64_t>(bitsPerSample - 1) << 36) |
                                totalSamples;
   appendBigEndian(body, packed, 8);
   body.resize(34, 0); // Unknown MD5.
   return body;
}

Bytes makeCueSheet() {
   Bytes body(128, 0);
   const std::string catalog = "1234567890123";
   std::copy(catalog.begin(), catalog.end(), body.begin());
   appendBigEndian(body, 88200, 8);
   body.push_back(0x80); // CD flag.
   body.resize(395, 0);
   body.push_back(2); // One audio track and the lead-out.
   appendBigEndian(body, 0, 8);
   body.push_back(1);
   appendString(body, "USABC1234567");
   body.push_back(0x40); // Audio, with pre-emphasis.
   body.resize(body.size() + 13, 0);
   body.push_back(2);
   for (std::uint8_t index = 0; index < 2; ++index) {
      appendBigEndian(body, static_cast<std::uint64_t>(index) * 588, 8);
      body.push_back(index);
      body.resize(body.size() + 3, 0);
   }
   appendBigEndian(body, 44100, 8);
   body.push_back(170);
   body.resize(body.size() + 12 + 1 + 13, 0);
   body.push_back(0);
   return body;
}

Bytes makeComments() {
   Bytes body;
   appendLittleEndian(body, 4);
   appendString(body, "test");
   const std::vector<std::string> comments = {
      "TITLE=Metadata fixture", "cUeShEeT=" + std::string(CUE_TEXT),
      "CUESHEET=REM second=value", "CUESHEET=", "NOTCUESHEET=ignored",
      "aRtIsT=Fixture Artist", "ARTIST=Second Artist", "ALBUM=Fixture Album"
   };
   appendLittleEndian(body, static_cast<std::uint32_t>(comments.size()));
   for (const auto& comment : comments) {
      appendLittleEndian(body, static_cast<std::uint32_t>(comment.size()));
      appendString(body, comment);
   }
   return body;
}

void writeFile(const std::filesystem::path& path, const Bytes& bytes) {
   std::ofstream output(path, std::ios::binary | std::ios::trunc);
   output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
   require(static_cast<bool>(output), "Failed to write fixture");
}

void expectFailure(const std::filesystem::path& path) {
   try {
      flaccd::FlacMetadataInspector{}.inspect(path);
   } catch (const std::runtime_error&) {
      return;
   }
   throw std::runtime_error("Expected metadata read failure: " + path.string());
}

void runTests(const std::filesystem::path& directory) {
   const auto descriptive = flaccd::parseCueTextMetadata(
      "PERFORMER \"Disc Artist\"\r\nTITLE \"Album = Title\"\r\n"
      "FILE \"disc.flac\" WAVE\n TRACK 01 AUDIO\n TITLE \"First Song\"\n"
      "PERFORMER \"Guest Artist\"\nTRACK 02 AUDIO\nTITLE Unquoted Song\n"
      "REM TITLE ignored\nTRACK bad AUDIO\nTITLE \"Ignore malformed track\"\n"
      "TRACK 03 AUDIO\nTITLE \"unterminated\nPERFORMER \"Bad\" trailing\n");
   require(descriptive.title == "Album = Title" && descriptive.performer == "Disc Artist",
           "Disc descriptive metadata mismatch");
   require(descriptive.tracks.size() == 3 && descriptive.tracks[0].number == 1 &&
           descriptive.tracks[0].title == "First Song" &&
           descriptive.tracks[0].performer == "Guest Artist" &&
           descriptive.tracks[1].number == 2 && descriptive.tracks[1].title == "Unquoted Song" &&
           descriptive.tracks[1].performer.empty() && descriptive.tracks[2].title.empty() &&
           descriptive.tracks[2].performer.empty(), "Track descriptive metadata mismatch");
   require(flaccd::parseCueTextMetadata("").tracks.empty(), "Empty CUE should have no tracks");
   std::filesystem::create_directories(directory);
   // Metadata-only fixtures deliberately contain no encoded audio frames.
   Bytes file = {'f', 'L', 'a', 'C'};
   appendBlock(file, 0, makeStreamInfo(44100, 2, 16, 44100), false);
   appendBlock(file, 5, makeCueSheet(), false);
   appendBlock(file, 4, makeComments(), true);
   writeFile(directory / "cue.flac", file);
   const auto metadata = flaccd::FlacMetadataInspector{}.inspect(directory / "cue.flac");
   require(metadata.sampleRate == 44100 && metadata.channels == 2 &&
           metadata.bitsPerSample == 16 && metadata.totalSamples == 44100, "STREAMINFO mismatch");
   require(metadata.cueSheets.size() == 1, "Missing native CUESHEET");
   const auto& cue = metadata.cueSheets.front();
   require(cue.catalogNumber == "1234567890123" && cue.leadIn == 88200 && cue.isCd,
           "Native CUESHEET header mismatch");
   require(cue.tracks.size() == 2, "Track count mismatch");
   const auto& track = cue.tracks.front();
   require(track.number == 1 && track.offset == 0 && track.isAudio && track.preEmphasis &&
           track.isrc == "USABC1234567", "Track metadata mismatch");
   require(track.indices.size() == 2 && track.indices[0].number == 0 &&
           track.indices[0].offset == 0 && track.indices[1].number == 1 &&
           track.indices[1].offset == 588, "INDEX 00/01 mismatch");
   require(cue.tracks[1].number == 170 && cue.tracks[1].offset == 44100 &&
           cue.tracks[1].indices.empty(), "Lead-out mismatch");
   require(metadata.embeddedCueSheets.size() == 3 && metadata.embeddedCueSheets[0] == CUE_TEXT &&
           metadata.embeddedCueSheets[1] == "REM second=value" &&
           metadata.embeddedCueSheets[2].empty(), "CUESHEET comment preservation mismatch");
   require(metadata.vorbisComments.size() == 8 &&
           metadata.vorbisComments[5] == "aRtIsT=Fixture Artist" &&
           metadata.vorbisComments[6] == "ARTIST=Second Artist" &&
           metadata.vorbisComments[7] == "ALBUM=Fixture Album", "Vorbis tag preservation mismatch");
   const auto nativeLayout = flaccd::TrackLayout::fromNative(metadata, metadata.cueSheets.front());
   const auto textLayout = flaccd::TrackLayout::fromText(metadata,
                                                         flaccd::parseCueTextMetadata(metadata.embeddedCueSheets.front()));
   require(nativeLayout.getLeadOut().discLba == 74 &&
           flaccd::compareTrackLayouts(nativeLayout, textLayout).empty(),
           "Inspected native/text layout integration mismatch");
   require(nativeLayout.queryLba(-1)->sourceSector == 0 && nativeLayout.queryLba(0)->sourceSector == 1,
           "Inspected first-track INDEX 00/01 mapping mismatch");

   file = {'f', 'L', 'a', 'C'};
   appendBlock(file, 0, makeStreamInfo(48000, 1, 24, 0), true);
   writeFile(directory / "plain.flac", file);
   const auto plain = flaccd::FlacMetadataInspector{}.inspect(directory / "plain.flac");
   require(plain.sampleRate == 48000 && plain.channels == 1 && plain.bitsPerSample == 24 &&
           plain.totalSamples == 0 && plain.cueSheets.empty() && plain.embeddedCueSheets.empty(),
           "Non-CD format or absent CUE metadata mismatch");

   writeFile(directory / "invalid.flac", {'n', 'o', 'p', 'e'});
   expectFailure(directory / "invalid.flac");
   expectFailure(directory / "missing.flac");
   file.pop_back();
   writeFile(directory / "truncated.flac", file);
   expectFailure(directory / "truncated.flac");
   file = {'f', 'L', 'a', 'C'};
   appendBlock(file, 0, makeStreamInfo(0, 2, 16, 0), true);
   writeFile(directory / "zero-rate.flac", file);
   expectFailure(directory / "zero-rate.flac");
   file = {'f', 'L', 'a', 'C'};
   appendBlock(file, 0, makeStreamInfo(44100, 2, 16, 44100), false);
   auto malformedCue = makeCueSheet();
   malformedCue.pop_back();
   appendBlock(file, 5, malformedCue, true);
   writeFile(directory / "malformed-cue.flac", file);
   expectFailure(directory / "malformed-cue.flac");
}

}

int main(int argumentCount, char* arguments[]) {
   try {
      require(argumentCount == 2, "Expected fixture directory argument");
      runTests(arguments[1]);
      std::cout << "Metadata tests passed\n";
   } catch (const std::exception& error) {
      std::cerr << error.what() << '\n';
      return 1;
   }
   return 0;
}
