#include "flaccd/CddaSectorReader.h"

#include <FLAC/metadata.h>
#include <FLAC/stream_encoder.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

constexpr std::uint32_t SECTOR_COUNT = 240;
constexpr std::uint32_t SAMPLE_COUNT = SECTOR_COUNT * flaccd::SAMPLES_PER_SECTOR;
constexpr std::string_view CUE_TEXT =
   "FILE fixture.flac WAVE\nTRACK 01 AUDIO\nFLAGS PRE DCP\n"
   "INDEX 00 00:00:00\nINDEX 01 00:00:02\nINDEX 02 00:00:07\n"
   "TRACK 02 AUDIO\nINDEX 00 00:00:25\nINDEX 01 00:00:28\nINDEX 02 00:00:40\n"
   "TRACK 03 AUDIO\nINDEX 01 00:00:60\n";

void require(bool condition, std::string_view message) {
   if (!condition) {
      throw std::runtime_error(std::string(message));
   }
}

template<typename Exception, typename Operation>
void expectError(Operation operation, std::string_view message) {
   try {
      operation();
   } catch (const Exception&) {
      return;
   }
   throw std::runtime_error("Expected error: " + std::string(message));
}

std::int32_t sampleValue(std::uint32_t sample, std::uint32_t channel) {
   // Includes both signed extrema, different channels, and nonzero pregap audio.
   const auto code = (sample * 7919U + channel * 32767U) & 0xffffU;
   return static_cast<std::int32_t>(code) - 32768;
}

flaccd::CddaSector expectedSector(std::uint32_t sourceSector) {
   flaccd::CddaSector result;
   for (std::uint32_t frame = 0; frame < flaccd::SAMPLES_PER_SECTOR; ++frame) {
      for (std::uint32_t channel = 0; channel < 2; ++channel) {
         const auto value = static_cast<std::uint16_t>(sampleValue(sourceSector * flaccd::SAMPLES_PER_SECTOR + frame, channel));
         const auto offset = (frame * 2 + channel) * 2;
         result[offset] = static_cast<std::byte>(value & 0xff);
         result[offset + 1] = static_cast<std::byte>(value >> 8);
      }
   }
   return result;
}

using Metadata = std::unique_ptr<FLAC__StreamMetadata, decltype(&FLAC__metadata_object_delete)>;

Metadata makeNativeCue() {
   Metadata block(FLAC__metadata_object_new(FLAC__METADATA_TYPE_CUESHEET), &FLAC__metadata_object_delete);
   require(static_cast<bool>(block), "Allocate native CUE");
   require(FLAC__metadata_object_cuesheet_resize_tracks(block.get(), 4), "Allocate CUE tracks");
   block->data.cue_sheet.is_cd = true;
   block->data.cue_sheet.lead_in = 88200;
   const std::array<std::uint64_t, 4> starts = {0, 25, 60, SECTOR_COUNT};
   for (std::uint32_t position = 0; position < 4; ++position) {
      auto& track = block->data.cue_sheet.tracks[position];
      track.number = position == 3 ? 170 : static_cast<FLAC__byte>(position + 1);
      track.offset = starts[position] * flaccd::SAMPLES_PER_SECTOR;
      if (position < 2) {
         require(FLAC__metadata_object_cuesheet_track_resize_indices(block.get(), position, 3), "Allocate CUE indexes");
         track.indices[0] = {0, 0};
         track.indices[1] = {(position == 0 ? 2 : 3) * flaccd::SAMPLES_PER_SECTOR, 1};
         track.indices[2] = {(position == 0 ? 7 : 15) * flaccd::SAMPLES_PER_SECTOR, 2};
      } else if (position == 2) {
         require(FLAC__metadata_object_cuesheet_track_resize_indices(block.get(), position, 1), "Allocate final index");
         track.indices[0] = {0, 1};
      }
   }
   block->data.cue_sheet.tracks[0].pre_emphasis = true;
   return block;
}

void createFlac(const std::filesystem::path& path,
                std::uint32_t blockSize,
                std::uint32_t sampleRate = 44100,
                std::uint32_t channels = 2,
                std::uint32_t bitsPerSample = 16,
                std::string_view cueText = CUE_TEXT,
                bool includeCue = true) {
   using Encoder = std::unique_ptr<FLAC__StreamEncoder, decltype(&FLAC__stream_encoder_delete)>;
   Metadata comments(FLAC__metadata_object_new(FLAC__METADATA_TYPE_VORBIS_COMMENT), &FLAC__metadata_object_delete);
   require(static_cast<bool>(comments), "Allocate Vorbis comments");
   // The setter copies from our string; no manually owned FLAC entry is needed.
   auto cueComment = "CUESHEET=" + std::string(cueText);
   FLAC__StreamMetadata_VorbisComment_Entry entry;
   entry.entry = reinterpret_cast<FLAC__byte*>(cueComment.data());
   entry.length = static_cast<FLAC__uint32>(cueComment.size());
   if (includeCue) {
      require(FLAC__metadata_object_vorbiscomment_append_comment(comments.get(), entry, true), "Set CUESHEET comment");
   }
   for (std::string comment : {"ALBUM=Fixture Album", "album=Fixture Album",
                                     "ALBUMARTIST=Fixture Album Artist", "ARTIST=Fixture Artist"}) {
      entry.entry = reinterpret_cast<FLAC__byte*>(comment.data());
      entry.length = static_cast<FLAC__uint32>(comment.size());
      require(FLAC__metadata_object_vorbiscomment_append_comment(comments.get(), entry, true), "Set descriptive tag");
   }
   auto native = makeNativeCue();
   std::array<FLAC__StreamMetadata*, 2> blocks = {comments.get(), native.get()};
   Encoder encoder(FLAC__stream_encoder_new(), &FLAC__stream_encoder_delete);
   require(static_cast<bool>(encoder), "Allocate FLAC encoder");
   require(FLAC__stream_encoder_set_streamable_subset(encoder.get(), false), "Set encoder subset");
   require(FLAC__stream_encoder_set_channels(encoder.get(), channels), "Set channels");
   require(FLAC__stream_encoder_set_bits_per_sample(encoder.get(), bitsPerSample), "Set bit depth");
   require(FLAC__stream_encoder_set_sample_rate(encoder.get(), sampleRate), "Set sample rate");
   require(FLAC__stream_encoder_set_blocksize(encoder.get(), blockSize), "Set block size");
   require(FLAC__stream_encoder_set_total_samples_estimate(encoder.get(), SAMPLE_COUNT), "Set sample count");
   require(FLAC__stream_encoder_set_metadata(encoder.get(), blocks.data(), includeCue ? 2 : 1), "Set encoder metadata");
   const auto initStatus = FLAC__stream_encoder_init_file(encoder.get(), path.string().c_str(), nullptr, nullptr);
   require(initStatus == FLAC__STREAM_ENCODER_INIT_STATUS_OK, "Initialize test FLAC encoder");
   std::vector<FLAC__int32> samples(1024 * channels);
   for (std::uint32_t start = 0; start < SAMPLE_COUNT;) {
      const auto count = std::min<std::uint32_t>(1024, SAMPLE_COUNT - start);
      for (std::uint32_t index = 0; index < count; ++index) {
         for (std::uint32_t channel = 0; channel < channels; ++channel) {
            samples[index * channels + channel] = sampleValue(start + index, channel);
         }
      }
      require(FLAC__stream_encoder_process_interleaved(encoder.get(), samples.data(), count), "Encode test samples");
      start += count;
   }
   require(FLAC__stream_encoder_finish(encoder.get()), "Finish test FLAC");
}

void checkSector(flaccd::CddaSectorReader& reader, std::uint32_t sourceSector) {
   const auto actual = reader.readSector(static_cast<std::int64_t>(sourceSector) - 2);
   require(actual == expectedSector(sourceSector), "Sector bytes differ from original PCM");
}

void testReads(const std::filesystem::path& path) {
   const auto metadata = flaccd::FlacMetadataInspector{}.inspect(path);
   const auto native = flaccd::TrackLayout::fromNative(metadata, metadata.cueSheets.front());
   const auto text = flaccd::TrackLayout::fromText(metadata,
                                                flaccd::parseCueTextMetadata(metadata.embeddedCueSheets.front()));
   require(flaccd::compareTrackLayouts(native, text).empty(), "Fixture native/text layout mismatch");
   for (const auto* layout : {&native, &text}) {
      flaccd::CddaSectorReader reader(path, *layout);
      checkSector(reader, 0);
      checkSector(reader, SECTOR_COUNT - 1);
      checkSector(reader, 0);
      for (std::uint32_t source = 0; source < SECTOR_COUNT; ++source) {
         checkSector(reader, source);
      }
      for (const std::int64_t boundary : {-2, 0, 5, 23, 26, 38, 58, 238}) {
         for (std::int64_t delta = -1; delta <= 1; ++delta) {
            const auto lba = boundary + delta;
            if (lba >= -2 && lba < 238) {
               require(reader.readSector(lba) == expectedSector(static_cast<std::uint32_t>(lba + 2)),
                       "Boundary sector differs from source PCM");
            } else {
               expectError<std::out_of_range>([&] { reader.readSector(lba); }, "outside boundary");
            }
         }
      }
      std::mt19937 generator(317);
      std::uniform_int_distribution<std::uint32_t> distribution(0, SECTOR_COUNT - 1);
      for (std::uint32_t iteration = 0; iteration < 300; ++iteration) {
         checkSector(reader, distribution(generator));
      }
      expectError<std::out_of_range>([&] { reader.readSector(std::numeric_limits<std::int64_t>::min()); }, "minimum LBA");
      expectError<std::out_of_range>([&] { reader.readSector(std::numeric_limits<std::int64_t>::max()); }, "maximum LBA");
      checkSector(reader, 0);
      auto moved = std::move(reader);
      checkSector(moved, 25);
      expectError<std::logic_error>([&] { reader.readSector(0); }, "moved-from reader");
      flaccd::CddaSectorReader assigned(path, *layout);
      checkSector(assigned, 0);
      assigned = std::move(moved);
      checkSector(assigned, 239);
      expectError<std::logic_error>([&] { moved.readSector(0); }, "move-assigned reader");
   }
}

void testGaps(const std::filesystem::path& path) {
   const auto metadata = flaccd::FlacMetadataInspector{}.inspect(path);
   const auto cueSheet = flaccd::parseCueTextMetadata("FILE fixture.flac WAVE\nTRACK 01 AUDIO\n"
                                                   "PREGAP 00:02:00\nINDEX 01 00:00:00\nPOSTGAP 00:00:03\n"
                                                   "TRACK 02 AUDIO\nPREGAP 00:00:05\nINDEX 01 00:00:25\n");
   const auto layout = flaccd::TrackLayout::fromText(metadata, cueSheet);
   flaccd::CddaSectorReader reader(path, layout);
   for (const auto lba : {-150, -1, 25, 27, 28, 32}) {
      expectError<std::invalid_argument>([&] { reader.readSector(lba); }, "source-less gap");
   }
   require(reader.readSector(0) == expectedSector(0) && reader.readSector(33) == expectedSector(25) &&
           reader.readSector(247) == expectedSector(239),
           "Disc-to-source mapping after gaps failed");
   expectError<std::out_of_range>([&] { reader.readSector(248); }, "gap layout lead-out");
}

void testErrors(const std::filesystem::path& directory) {
   const auto original = directory / "fixture-4096.flac";
   const auto metadata = flaccd::FlacMetadataInspector{}.inspect(original);
   const auto layout = flaccd::TrackLayout::fromNative(metadata, metadata.cueSheets.front());
   for (std::uint32_t variant = 0; variant < 3; ++variant) {
      const auto path = directory / ("unsupported-" + std::to_string(variant) + ".flac");
      createFlac(path, 4096, variant == 0 ? 48000 : 44100, variant == 1 ? 1 : 2, variant == 2 ? 24 : 16);
      expectError<std::invalid_argument>([&] { flaccd::CddaSectorReader reader(path, layout); }, "unsupported audio format");
   }
   auto wrongMetadata = metadata;
   wrongMetadata.totalSamples -= flaccd::SAMPLES_PER_SECTOR;
   auto wrongCue = metadata.cueSheets.front();
   wrongCue.tracks.back().offset = wrongMetadata.totalSamples;
   const auto wrongLayout = flaccd::TrackLayout::fromNative(wrongMetadata, wrongCue);
   expectError<std::invalid_argument>([&] { flaccd::CddaSectorReader reader(original, wrongLayout); }, "layout length mismatch");
   expectError<std::runtime_error>([&] { flaccd::CddaSectorReader reader(directory / "missing.flac", layout); }, "missing file");

   const auto truncated = directory / "truncated.flac";
   std::filesystem::copy_file(original, truncated, std::filesystem::copy_options::overwrite_existing);
   std::filesystem::resize_file(truncated, std::filesystem::file_size(truncated) / 2);
   flaccd::CddaSectorReader reader(truncated, layout);
   expectError<std::runtime_error>([&] { reader.readSector(237); }, "truncated audio");
   expectError<std::runtime_error>([&] { reader.readSector(0); }, "failed decoder must remain failed");
   flaccd::CddaSectorReader sequential(truncated, layout);
   std::uint32_t completeSectors = 0;
   const auto readUntilFailure = [&] {
      for (; completeSectors < SECTOR_COUNT; ++completeSectors) {
         checkSector(sequential, completeSectors);
      }
   };
   expectError<std::runtime_error>(readUntilFailure, "sequential read into truncated audio");
   require(completeSectors > 0 && completeSectors < SECTOR_COUNT, "Truncation did not interrupt sequential decoding");

   const auto corrupt = directory / "corrupt.flac";
   std::filesystem::copy_file(original, corrupt, std::filesystem::copy_options::overwrite_existing);
   std::fstream file(corrupt, std::ios::binary | std::ios::in | std::ios::out);
   require(static_cast<bool>(file), "Open corrupt fixture");
   file.seekg(4);
   bool isLast = false;
   while (!isLast) {
      std::array<unsigned char, 4> header;
      file.read(reinterpret_cast<char*>(header.data()), header.size());
      require(static_cast<bool>(file), "Read metadata header");
      isLast = (header[0] & 0x80) != 0;
      const auto size = (static_cast<std::uint32_t>(header[1]) << 16) |
                        (static_cast<std::uint32_t>(header[2]) << 8) | header[3];
      file.seekg(size, std::ios::cur);
   }
   const auto damagePosition = file.tellg() + std::streamoff(50);
   file.seekg(damagePosition);
   char byte = 0;
   file.get(byte);
   file.seekp(damagePosition);
   file.put(static_cast<char>(static_cast<unsigned char>(byte) ^ 0x20));
   file.close();
   flaccd::CddaSectorReader damaged(corrupt, layout);
   expectError<std::runtime_error>([&] { damaged.readSector(-2); }, "corruption must not be returned as silence");
}

}

int main(int argumentCount, char* arguments[]) {
   try {
      require(argumentCount == 2, "Expected fixture directory");
      const std::filesystem::path directory(arguments[1]);
      std::filesystem::create_directories(directory);
      createFlac(directory / "fixture-no-cue.flac", 4096, 44100, 2, 16, CUE_TEXT, false);
      for (const auto blockSize : {192U, 4096U, 16384U, 65535U}) {
         const auto path = directory / ("fixture-" + std::to_string(blockSize) + ".flac");
         createFlac(path, blockSize);
         testReads(path);
      }
      std::ofstream reference(directory / "original.raw", std::ios::binary | std::ios::trunc);
      for (std::uint32_t sector = 0; sector < SECTOR_COUNT; ++sector) {
         const auto bytes = expectedSector(sector);
         reference.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
      }
      reference.close();
      require(static_cast<bool>(reference), "Write original PCM reference");
      testGaps(directory / "fixture-4096.flac");
      testErrors(directory);
      createFlac(directory / "fixture-no-first-pregap.flac", 4096, 44100, 2, 16,
                 "FILE fixture.flac WAVE\nTRACK 01 AUDIO\nFLAGS PRE DCP\n"
                 "INDEX 01 00:00:00\nTRACK 02 AUDIO\nINDEX 00 00:00:25\nINDEX 01 00:00:28\n"
                 "TRACK 03 AUDIO\nINDEX 01 00:00:60\n");
      createFlac(directory / "fixture-gap.flac", 4096, 44100, 2, 16,
                 "FILE fixture.flac WAVE\nTRACK 01 AUDIO\nPREGAP 00:00:03\n"
                 "INDEX 01 00:00:00\nPOSTGAP 00:00:01\n");
      std::cout << "CDDA reader tests passed (block sizes 192, 4096, 16384, 65535)\n";
   } catch (const std::exception& error) {
      std::cerr << error.what() << '\n';
      return 1;
   }
   return 0;
}
