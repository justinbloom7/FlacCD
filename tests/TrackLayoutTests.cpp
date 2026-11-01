#include "flaccd/TrackLayout.h"

#include <functional>
#include <cstdint>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

void require(bool condition, std::string_view message) {
   if (!condition) {
      throw std::runtime_error(std::string(message));
   }
}

void expectInvalid(const std::function<void()>& operation, std::string_view message) {
   try {
      operation();
   } catch (const std::invalid_argument&) {
      return;
   }
   throw std::runtime_error("Expected rejection: " + std::string(message));
}

flaccd::FlacMetadata makeMetadata() {
   flaccd::FlacMetadata metadata;
   metadata.sampleRate = 44100;
   metadata.channels = 2;
   metadata.bitsPerSample = 16;
   metadata.totalSamples = 1000 * flaccd::SAMPLES_PER_SECTOR;
   return metadata;
}

flaccd::NativeCueSheet makeNative() {
   flaccd::NativeCueSheet cue;
   cue.isCd = true;
   cue.leadIn = 88200;
   cue.catalogNumber = "1234567890123";
   flaccd::CueTrack first;
   first.number = 1;
   first.preEmphasis = true;
   first.isrc = "USABC1234567";
   first.indices = {{1, 0}, {2, 100 * flaccd::SAMPLES_PER_SECTOR}};
   flaccd::CueTrack second;
   second.number = 2;
   second.offset = 300 * flaccd::SAMPLES_PER_SECTOR;
   second.indices = {{0, 0}, {1, 25 * flaccd::SAMPLES_PER_SECTOR},
                     {2, 100 * flaccd::SAMPLES_PER_SECTOR}};
   flaccd::CueTrack leadOut;
   leadOut.number = 170;
   leadOut.offset = 1000 * flaccd::SAMPLES_PER_SECTOR;
   cue.tracks = {first, second, leadOut};
   return cue;
}

flaccd::TrackLayout textLayout(std::string_view text) {
   return flaccd::TrackLayout::fromText(makeMetadata(), flaccd::parseCueTextMetadata(text));
}

void checkPosition(const flaccd::TrackLayout& layout, std::int64_t lba,
                   std::uint32_t track, std::optional<std::uint32_t> index,
                   std::optional<std::uint64_t> source, flaccd::RegionKind kind) {
   const auto result = layout.queryLba(lba);
   require(result.has_value(), "Missing LBA result");
   require(result->trackNumber == track && result->indexNumber == index &&
           result->sourceSector == source && result->kind == kind, "Wrong LBA mapping");
}

void testEquivalentLayouts() {
   const auto metadata = makeMetadata();
   const auto native = flaccd::TrackLayout::fromNative(metadata, makeNative());
   const auto text = textLayout(
      "CATALOG 1234567890123\r\nPERFORMER \"Artist\"\r\nTITLE \"Album\"\r\n"
      "FILE \"album.flac\" WAVE\r\nTRACK 01 AUDIO\r\n"
      "TITLE \"First\"\r\nFLAGS PRE DCP\r\nISRC USABC1234567\r\n"
      "INDEX 01 00:00:00\r\nINDEX 02 00:01:25\r\n"
      "TRACK 02 AUDIO\r\nTITLE \"Second\"\r\n"
      "INDEX 00 00:04:00\r\nINDEX 01 00:04:25\r\nINDEX 02 00:05:25\r\n");
   require(flaccd::compareTrackLayouts(native, text).empty(), "Equivalent layouts conflict");
   require(native.getTracks().size() == 2 && native.getTracks()[1].storedPregapSectors == 25,
           "Stored pregap mismatch");
   require(native.getLeadOut().fromNativeMetadata && !text.getLeadOut().fromNativeMetadata &&
           native.getLeadOut().discLba == 1000 && native.getLeadOut().sourceSector == 1000,
           "Lead-out provenance mismatch");
   require(native.getLeadInSamples() == 88200 && !text.getLeadInSamples(), "Lead-in mismatch");
   require(!native.getTracks()[0].flags.digitalCopyPermitted &&
           text.getTracks()[0].flags.digitalCopyPermitted == true, "DCP unknown/true mismatch");
   require(text.getTracks()[0].title == "First" && text.getTracks()[1].performer == "Artist",
           "Descriptive parser reuse mismatch");
   for (const auto* layout : {&native, &text}) {
      checkPosition(*layout, 0, 1, 1, 0, flaccd::RegionKind::SourceAudio);
      checkPosition(*layout, 99, 1, 1, 99, flaccd::RegionKind::SourceAudio);
      checkPosition(*layout, 100, 1, 2, 100, flaccd::RegionKind::SourceAudio);
      checkPosition(*layout, 299, 1, 2, 299, flaccd::RegionKind::SourceAudio);
      checkPosition(*layout, 300, 2, 0, 300, flaccd::RegionKind::SourceAudio);
      checkPosition(*layout, 324, 2, 0, 324, flaccd::RegionKind::SourceAudio);
      checkPosition(*layout, 325, 2, 1, 325, flaccd::RegionKind::SourceAudio);
      checkPosition(*layout, 400, 2, 2, 400, flaccd::RegionKind::SourceAudio);
      checkPosition(*layout, 999, 2, 2, 999, flaccd::RegionKind::SourceAudio);
      require(!layout->queryLba(-1) && !layout->queryLba(1000) &&
              !layout->queryLba(std::numeric_limits<std::int64_t>::max()), "Outside-disc lookup mismatch");
   }
}

void testDeclaredGaps() {
   const auto layout = textLayout(
      "FILE disc.flac WAVE\nTRACK 01 AUDIO\nPREGAP 00:02:00\nINDEX 01 00:00:00\n"
      "POSTGAP 00:00:05\nTRACK 02 AUDIO\nPREGAP 00:00:10\nINDEX 01 00:04:00\n"
      "POSTGAP 00:00:07\n");
   require(layout.getTracks()[0].startLba == -150 && layout.getTracks()[0].endLba == 305 &&
           layout.getTracks()[1].startLba == 305 && layout.getLeadOut().discLba == 1022,
           "Inserted gap coordinates mismatch");
   require(layout.getTracks()[0].indices[0].origin == flaccd::IndexOrigin::PregapDirective &&
           !layout.getTracks()[0].indices[0].sourceSector, "Gap index provenance mismatch");
   checkPosition(layout, -150, 1, 0, std::nullopt, flaccd::RegionKind::Pregap);
   checkPosition(layout, -1, 1, 0, std::nullopt, flaccd::RegionKind::Pregap);
   checkPosition(layout, 0, 1, 1, 0, flaccd::RegionKind::SourceAudio);
   checkPosition(layout, 299, 1, 1, 299, flaccd::RegionKind::SourceAudio);
   checkPosition(layout, 300, 1, std::nullopt, std::nullopt, flaccd::RegionKind::Postgap);
   checkPosition(layout, 304, 1, std::nullopt, std::nullopt, flaccd::RegionKind::Postgap);
   checkPosition(layout, 305, 2, 0, std::nullopt, flaccd::RegionKind::Pregap);
   checkPosition(layout, 314, 2, 0, std::nullopt, flaccd::RegionKind::Pregap);
   checkPosition(layout, 315, 2, 1, 300, flaccd::RegionKind::SourceAudio);
   checkPosition(layout, 1014, 2, 1, 999, flaccd::RegionKind::SourceAudio);
   checkPosition(layout, 1015, 2, std::nullopt, std::nullopt, flaccd::RegionKind::Postgap);
   checkPosition(layout, 1021, 2, std::nullopt, std::nullopt, flaccd::RegionKind::Postgap);
   require(!layout.queryLba(-151) && !layout.queryLba(1022), "Gap layout range mismatch");

   const auto stored = textLayout("FILE disc.flac WAVE\nTRACK 01 AUDIO\nINDEX 00 00:00:00\nINDEX 01 00:00:10\n");
   checkPosition(stored, -10, 1, 0, 0, flaccd::RegionKind::SourceAudio);
   checkPosition(stored, -1, 1, 0, 9, flaccd::RegionKind::SourceAudio);
   checkPosition(stored, 0, 1, 1, 10, flaccd::RegionKind::SourceAudio);
   require(stored.getLeadOut().discLba == 990, "Stored first pregap lead-out mismatch");
   const auto zeroGap = textLayout(
      "FILE disc.flac WAVE\nTRACK 01 AUDIO\nINDEX 00 00:00:00\nINDEX 01 00:00:00\n");
   require(zeroGap.getTracks()[0].indices.size() == 2, "Coincident indexes were lost");
   checkPosition(zeroGap, 0, 1, 1, 0, flaccd::RegionKind::SourceAudio);
   const auto absent = textLayout("FILE disc.flac FLAC\nTRACK 01 AUDIO\nINDEX 01 00:00:00\n");
   require(absent.getTracks()[0].indices.size() == 1 && !absent.queryLba(-1), "Invented missing pregap");
   const auto lowercase = textLayout(
      "file disc.flac wave\ntrack 01 audio\nflags pre dcp\nindex 01 00:00:00\n");
   require(lowercase.getTracks()[0].flags.preEmphasis &&
           lowercase.getTracks()[0].flags.digitalCopyPermitted == true, "Case-insensitive syntax mismatch");
   std::vector<bool> visited(1000, false);
   for (std::int64_t lba = -150; lba < 1022; ++lba) {
      const auto mapping = layout.queryLba(lba);
      require(mapping.has_value(), "Unmapped disc sector");
      if (mapping->sourceSector) {
         require(*mapping->sourceSector < visited.size() && !visited[*mapping->sourceSector],
                 "Source audio duplicated or out of bounds");
         visited[*mapping->sourceSector] = true;
      } else {
         require(mapping->kind != flaccd::RegionKind::SourceAudio, "Missing source for audio region");
      }
   }
   for (bool found : visited) {
      require(found, "Source audio lost when inserting gaps");
   }
}

void testInvalidText() {
   constexpr std::string_view PREFIX = "FILE disc.flac WAVE\n";
   const std::vector<std::string> invalid = {
      "", "TRACK 01 AUDIO\n", "TRACK 01 AUDIO\nINDEX 00 00:00:00\n",
      "TRACK 01 AUDIO\nINDEX 02 00:00:00\n",
      "TRACK 01 AUDIO\nINDEX 01 00:00:00\nINDEX 01 00:00:10\n",
      "TRACK 01 AUDIO\nINDEX 01 00:00:00\nINDEX 03 00:00:10\n",
      "TRACK 01 AUDIO\nINDEX 00 00:00:10\nINDEX 01 00:00:00\n",
      "TRACK 01 AUDIO\nINDEX 01 00:00:01\n",
      "TRACK 01 AUDIO\nINDEX 01 00:00:00\nTRACK 01 AUDIO\nINDEX 01 00:01:00\n",
      "TRACK 02 AUDIO\nINDEX 01 00:00:00\nTRACK 01 AUDIO\nINDEX 01 00:01:00\n",
      "TRACK 01 AUDIO\nINDEX 01 00:00:00\nTRACK 03 AUDIO\nINDEX 01 00:01:00\n",
      "TRACK 01 AUDIO\nINDEX 01 00:00:00\nINDEX 02 00:02:00\n"
      "TRACK 02 AUDIO\nINDEX 01 00:01:00\n",
      "TRACK 01 AUDIO\nINDEX 01 00:00:00\nTRACK 02 AUDIO\nINDEX 01 00:00:00\n",
      "TRACK 01 AUDIO\nINDEX 01 00:00:00\nTRACK 02 AUDIO\nINDEX 01 00:13:25\n",
      "TRACK 01 MODE1/2352\nINDEX 01 00:00:00\n",
      "TRACK 01 AUDIO\nPREGAP 00:00:01\nINDEX 00 00:00:00\nINDEX 01 00:00:10\n",
      "TRACK 01 AUDIO\nINDEX 01 00:00:00\nPREGAP 00:00:10\n",
      "TRACK 01 AUDIO\nPOSTGAP 00:00:10\nINDEX 01 00:00:00\n",
      "TRACK 01 AUDIO\nINDEX 01 00:00:00\nPOSTGAP 00:00:10\nINDEX 02 00:01:00\n",
      "TRACK 01 AUDIO\nFLAGS 4CH\nINDEX 01 00:00:00\n",
      "TRACK 01 AUDIO\nFLAGS PRE PRE\nINDEX 01 00:00:00\n",
      "TRACK 01 AUDIO\nFLAGS DCP\nFLAGS PRE\nINDEX 01 00:00:00\n",
      "TRACK 01 AUDIO\nISRC invalid\nINDEX 01 00:00:00\n",
      "TRACK 01 AUDIO\nINDEX 01 00:00:00\nISRC USABC1234567\n",
      "TRACK 01 AUDIO\nINDEX 01 00:00:00\nTITLE \"misplaced\"\n",
      "TRACK 01 AUDIO\nISRC \"\"\nINDEX 01 00:00:00\n",
      "TRACK 01 AUDIO\nTITLE \"unclosed\nINDEX 01 00:00:00\n",
      "TRACK 01 AUDIO\nINDEX 01 00:00:00\nFILE other.flac WAVE\n",
      "INDEX 01 00:00:00\n", "CDTEXTFILE disc.cdt\n",
      "TRACK 01 AUDIO\nBOGUS ignored\nINDEX 01 00:00:00\n",
      "TRACK 01 AUDIO\nPREGAP 999999999999999999:00:00\nINDEX 01 00:00:00\n"
   };
   for (const auto& body : invalid) {
      expectInvalid([&] { textLayout(std::string(PREFIX) + body); }, body);
   }
   expectInvalid([] { textLayout("TRACK 01 AUDIO\nINDEX 01 00:00:00\n"); }, "missing FILE");
   expectInvalid([] { textLayout("FILE disc.bin BINARY\nTRACK 01 AUDIO\nINDEX 01 00:00:00\n"); }, "BINARY input");
   expectInvalid([] { textLayout("CATALOG 123\nFILE disc.flac WAVE\nTRACK 01 AUDIO\nINDEX 01 00:00:00\n"); }, "catalog");
   expectInvalid([] { textLayout("FILE \"disc.flac\"WAVE\nTRACK 01 AUDIO\nINDEX 01 00:00:00\n"); }, "FILE separation");
   expectInvalid([] { textLayout("FILE disc.flac WAVE\nTITLE \"misplaced\"\nTRACK 01 AUDIO\nINDEX 01 00:00:00\n"); }, "disc field scope");
   for (const auto timestamp : {"0:00:00", "00:60:00", "00:00:75", "-1:00:00", "00:00",
                                "00:00:00:00", "00:00:0x", "18446744073709551615:00:00"}) {
      expectInvalid([&] { flaccd::parseCueTimestamp(timestamp); }, timestamp);
   }
   require(flaccd::parseCueTimestamp("01:02:03") == 4653 &&
           flaccd::parseCueTimestamp("100:00:00") == 450000, "Exact timestamp conversion mismatch");
}

void testInvalidNative() {
   const auto metadata = makeMetadata();
   using Mutation = std::function<void(flaccd::NativeCueSheet&)>;
   const std::vector<Mutation> mutations = {
      [](auto& cue) { cue.tracks.clear(); },
      [](auto& cue) { cue.isCd = false; },
      [](auto& cue) { cue.leadIn = 0; },
      [](auto& cue) { ++cue.leadIn; },
      [](auto& cue) { cue.tracks.pop_back(); },
      [](auto& cue) { cue.tracks.back().number = 255; },
      [](auto& cue) { cue.tracks.back().indices = {{1, 0}}; },
      [](auto& cue) { cue.tracks.back().offset -= flaccd::SAMPLES_PER_SECTOR; },
      [](auto& cue) { cue.tracks[0].number = 0; },
      [](auto& cue) { cue.tracks[1].number = 1; },
      [](auto& cue) { cue.tracks[0].isAudio = false; },
      [](auto& cue) { cue.tracks[0].indices.clear(); },
      [](auto& cue) { cue.tracks[0].indices[0].number = 0; },
      [](auto& cue) { cue.tracks[0].indices[0].offset = 588; },
      [](auto& cue) { ++cue.tracks[1].offset; },
      [](auto& cue) { ++cue.tracks[1].indices[1].offset; },
      [](auto& cue) {
         cue.tracks[1].indices[1].offset = (std::numeric_limits<std::uint64_t>::max() /
                                          flaccd::SAMPLES_PER_SECTOR) * flaccd::SAMPLES_PER_SECTOR;
      },
      [](auto& cue) { cue.tracks[1].indices[1].offset = 1000 * flaccd::SAMPLES_PER_SECTOR; },
      [](auto& cue) { cue.tracks[1].offset = 50 * flaccd::SAMPLES_PER_SECTOR; },
      [](auto& cue) { cue.tracks[0].isrc = "bad"; }
   };
   for (const auto& mutate : mutations) {
      auto cue = makeNative();
      mutate(cue);
      expectInvalid([&] { flaccd::TrackLayout::fromNative(metadata, cue); }, "native mutation");
   }
   for (int variant = 0; variant < 5; ++variant) {
      auto invalid = metadata;
      if (variant == 0) {
         invalid.totalSamples = 0;
      } else if (variant == 1) {
         ++invalid.totalSamples;
      } else if (variant == 2) {
         invalid.sampleRate = 48000;
      } else if (variant == 3) {
         invalid.channels = 1;
      } else {
         invalid.bitsPerSample = 24;
      }
      expectInvalid([&] { flaccd::TrackLayout::fromNative(invalid, makeNative()); }, "STREAMINFO");
      expectInvalid([&] {
         flaccd::TrackLayout::fromText(invalid, flaccd::parseCueTextMetadata(
            "FILE disc.flac WAVE\nTRACK 01 AUDIO\nINDEX 01 00:00:00\n"));
      }, "text STREAMINFO");
   }
   auto overflow = flaccd::parseCueTextMetadata("FILE disc.flac WAVE\nTRACK 01 AUDIO\nINDEX 01 00:00:00\n");
   overflow.tracks[0].pregap = std::numeric_limits<std::int64_t>::max();
   expectInvalid([&] { flaccd::TrackLayout::fromText(metadata, overflow); }, "signed LBA overflow");
}

void testConflicts() {
   const auto original = flaccd::TrackLayout::fromNative(makeMetadata(), makeNative());
   auto changed = makeNative();
   changed.tracks[1].indices[1].offset += flaccd::SAMPLES_PER_SECTOR;
   require(!flaccd::compareTrackLayouts(original,
           flaccd::TrackLayout::fromNative(makeMetadata(), changed)).empty(), "Offset conflict not reported");
   changed = makeNative();
   changed.tracks[0].preEmphasis = false;
   require(!flaccd::compareTrackLayouts(original,
           flaccd::TrackLayout::fromNative(makeMetadata(), changed)).empty(), "Flag conflict not reported");
   const auto plain = textLayout("FILE disc.flac WAVE\nTRACK 01 AUDIO\nINDEX 01 00:00:00\n");
   const auto copy = textLayout("FILE disc.flac WAVE\nTRACK 01 AUDIO\nFLAGS DCP\nINDEX 01 00:00:00\n");
   require(!flaccd::compareTrackLayouts(plain, copy).empty(), "DCP conflict not reported");
}

}

int main() {
   try {
      testEquivalentLayouts();
      testDeclaredGaps();
      testInvalidText();
      testInvalidNative();
      testConflicts();
      std::cout << "Track layout tests passed\n";
   } catch (const std::exception& error) {
      std::cerr << error.what() << '\n';
      return 1;
   }
   return 0;
}
