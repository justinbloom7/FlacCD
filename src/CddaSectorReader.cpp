#include "flaccd/CddaSectorReader.h"

#include "flaccd/FlacMetadataInspector.h"

#include <FLAC/stream_decoder.h>

#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>
#include <string>

namespace flaccd {
namespace {

constexpr std::size_t CHANNEL_COUNT = 2;
constexpr std::size_t MAX_FRAME_SAMPLES = 65535;
static_assert(BYTES_PER_SECTOR == SAMPLES_PER_SECTOR * CHANNEL_COUNT * sizeof(std::int16_t));

struct DecoderDeleter {
   void operator()(FLAC__StreamDecoder* decoder) const noexcept {
      FLAC__stream_decoder_finish(decoder);
      FLAC__stream_decoder_delete(decoder);
   }
};

}

class CddaSectorReader::Impl {
public:
   Impl(const std::filesystem::path& path, const TrackLayout& discLayout) : layout(discLayout) {
      const auto metadata = FlacMetadataInspector{}.inspect(path);
      if (metadata.sampleRate != 44100 || metadata.channels != 2 || metadata.bitsPerSample != 16) {
         throw std::invalid_argument("CDDA reader requires 44100 Hz, 16-bit, stereo FLAC; conversion is unsupported");
      }
      if (metadata.totalSamples == 0 || metadata.totalSamples % SAMPLES_PER_SECTOR != 0) {
         throw std::invalid_argument("CDDA reader requires a known, sector-aligned sample count");
      }
      totalSamples = metadata.totalSamples;
      if (layout.getLeadOut().sourceSector != totalSamples / SAMPLES_PER_SECTOR) {
         throw std::invalid_argument("layout source length does not match FLAC STREAMINFO");
      }
      decoder.reset(FLAC__stream_decoder_new());
      if (!decoder) {
         throw std::runtime_error("unable to allocate FLAC decoder");
      }
      // Random access cannot validate the whole-stream MD5. Frame decoding
      // errors are fatal, including CRC errors that libFLAC may conceal as silence.
      FLAC__stream_decoder_set_md5_checking(decoder.get(), false);
      const auto status = FLAC__stream_decoder_init_file(decoder.get(),
                                                       path.string().c_str(),
                                                       &writeCallback,
                                                       &metadataCallback,
                                                       &errorCallback,
                                                       this);
      if (status != FLAC__STREAM_DECODER_INIT_STATUS_OK) {
         throw std::runtime_error(std::string("unable to initialize FLAC decoder: ") +
                                  FLAC__StreamDecoderInitStatusString[status]);
      }
      const bool success = FLAC__stream_decoder_process_until_end_of_metadata(decoder.get());
      checkDecoder(success, "unable to read decoder metadata");
      if (!foundStreamInfo) {
         fail("decoder did not provide STREAMINFO");
      }
   }

   CddaSector readSector(std::int64_t discLba) {
      if (failed) {
         throw std::runtime_error("FLAC decoder previously failed; reopen the reader");
      }
      const auto position = layout.queryLba(discLba);
      if (!position) {
         throw std::out_of_range("LBA is outside the disc layout (lead-out is exclusive)");
      }
      if (!position->sourceSector || position->kind != RegionKind::SourceAudio) {
         throw std::invalid_argument("LBA belongs to a declared PREGAP/POSTGAP with no source audio");
      }
      if (*position->sourceSector >= totalSamples / SAMPLES_PER_SECTOR) {
         throw std::out_of_range("LBA maps outside the FLAC source audio");
      }
      const auto firstSample = *position->sourceSector * SAMPLES_PER_SECTOR;
      CddaSector result;
      std::size_t copiedFrames = 0;
      while (copiedFrames < SAMPLES_PER_SECTOR) {
         const auto sample = firstSample + copiedFrames;
         if (frameSamples == 0 || sample < frameStart || sample - frameStart >= frameSamples) {
            loadFrame(sample);
         }
         const auto offset = static_cast<std::size_t>(sample - frameStart);
         const auto count = std::min<std::size_t>(SAMPLES_PER_SECTOR - copiedFrames, frameSamples - offset);
         for (std::size_t index = 0; index < count * CHANNEL_COUNT; ++index) {
            const auto value = static_cast<std::uint16_t>(frameBuffer[offset * CHANNEL_COUNT + index]);
            const auto destination = (copiedFrames * CHANNEL_COUNT + index) * sizeof(std::int16_t);
            result[destination] = static_cast<std::byte>(value & 0xff);
            result[destination + 1] = static_cast<std::byte>(value >> 8);
         }
         copiedFrames += count;
      }
      return result;
   }

private:
   [[noreturn]] void fail(const std::string& message) {
      failed = true;
      frameSamples = 0;
      throw std::runtime_error(message);
   }

   void checkDecoder(bool success, const char* context) {
      if (callbackError) {
         fail(std::string(context) + ": " + callbackError);
      }
      if (!success) {
         fail(std::string(context) + ": " + FLAC__stream_decoder_get_resolved_state_string(decoder.get()));
      }
   }

   void loadFrame(std::uint64_t sample) {
      frameSamples = 0;
      if (sample != nextSample) {
         nextSample = sample;
         const bool success = FLAC__stream_decoder_seek_absolute(decoder.get(), sample);
         checkDecoder(success, "FLAC sample seek failed");
      }
      if (frameSamples == 0) {
         const bool success = FLAC__stream_decoder_process_single(decoder.get());
         checkDecoder(success, "FLAC frame decode failed");
      }
      if (frameSamples == 0) {
         fail("premature FLAC end-of-stream while reading a CDDA sector");
      }
      if (sample < frameStart || sample - frameStart >= frameSamples) {
         fail("FLAC decoder did not return the requested source sample");
      }
   }

   static FLAC__StreamDecoderWriteStatus writeCallback(const FLAC__StreamDecoder*,
                                                       const FLAC__Frame* frame,
                                                       const FLAC__int32* const channels[],
                                                       void* clientData) noexcept {
      auto& self = *static_cast<Impl*>(clientData);
      const auto& header = frame->header;
      if (header.sample_rate != 44100 || header.channels != 2 || header.bits_per_sample != 16 ||
          header.blocksize == 0 || header.blocksize > MAX_FRAME_SAMPLES ||
          header.number_type != FLAC__FRAME_NUMBER_TYPE_SAMPLE_NUMBER) {
         self.callbackError = "unexpected FLAC frame format";
         return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
      }
      const auto start = header.number.sample_number;
      if (start != self.nextSample || start > self.totalSamples || header.blocksize > self.totalSamples - start) {
         self.callbackError = "FLAC frame sample range is discontinuous or exceeds STREAMINFO";
         return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
      }
      for (std::size_t index = 0; index < header.blocksize; ++index) {
         for (std::size_t channel = 0; channel < CHANNEL_COUNT; ++channel) {
            const auto value = channels[channel][index];
            if (value < std::numeric_limits<std::int16_t>::min() ||
                value > std::numeric_limits<std::int16_t>::max()) {
               self.callbackError = "decoded sample exceeds signed 16-bit range";
               return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
            }
            self.frameBuffer[index * CHANNEL_COUNT + channel] = static_cast<std::int16_t>(value);
         }
      }
      self.frameStart = start;
      self.frameSamples = header.blocksize;
      self.nextSample = start + header.blocksize;
      return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
   }

   static void metadataCallback(const FLAC__StreamDecoder*,
                                const FLAC__StreamMetadata* metadata,
                                void* clientData) noexcept {
      auto& self = *static_cast<Impl*>(clientData);
      if (metadata->type == FLAC__METADATA_TYPE_STREAMINFO) {
         const auto& info = metadata->data.stream_info;
         self.foundStreamInfo = true;
         if (info.sample_rate != 44100 || info.channels != 2 || info.bits_per_sample != 16 ||
             info.total_samples != self.totalSamples) {
            self.callbackError = "decoder STREAMINFO does not match the inspected FLAC";
         }
      }
   }

   static void errorCallback(const FLAC__StreamDecoder*,
                             FLAC__StreamDecoderErrorStatus status,
                             void* clientData) noexcept {
      auto& self = *static_cast<Impl*>(clientData);
      self.callbackError = FLAC__StreamDecoderErrorStatusString[status];
   }

   TrackLayout layout;
   std::uint64_t totalSamples = 0;
   std::array<std::int16_t, MAX_FRAME_SAMPLES * CHANNEL_COUNT> frameBuffer;
   std::uint64_t frameStart = 0;
   std::uint64_t nextSample = 0;
   std::size_t frameSamples = 0;
   const char* callbackError = nullptr;
   bool foundStreamInfo = false;
   bool failed = false;
   // Destroy first so callbacks remain valid through finish().
   std::unique_ptr<FLAC__StreamDecoder, DecoderDeleter> decoder;
};

CddaSectorReader::CddaSectorReader(const std::filesystem::path& path, const TrackLayout& layout)
   : impl(std::make_unique<Impl>(path, layout)) {
}

CddaSectorReader::~CddaSectorReader() = default;
CddaSectorReader::CddaSectorReader(CddaSectorReader&& other) noexcept = default;
CddaSectorReader& CddaSectorReader::operator=(CddaSectorReader&& other) noexcept = default;

CddaSector CddaSectorReader::readSector(std::int64_t discLba) {
   if (!impl) {
      throw std::logic_error("cannot read from a moved-from CDDA reader");
   }
   return impl->readSector(discLba);
}

}
