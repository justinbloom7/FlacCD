#include "flaccd/FlacMetadataInspector.h"

#include <FLAC/metadata.h>

#include <cstdio>
#include <fstream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace flaccd {
namespace {

// Standard path-aware streams keep Windows Unicode filenames out of narrow
// C-runtime code pages. libFLAC owns neither the stream nor these callbacks.
std::size_t readMetadata(void* destination, std::size_t size, std::size_t count, void* handle) noexcept {
   if (size == 0 || count > static_cast<std::size_t>(std::numeric_limits<std::streamsize>::max()) / size) {
      return 0;
   }
   auto& stream = *static_cast<std::ifstream*>(handle);
   stream.read(static_cast<char*>(destination), static_cast<std::streamsize>(size * count));
   return static_cast<std::size_t>(stream.gcount()) / size;
}

int seekMetadata(void* handle, FLAC__int64 offset, int origin) noexcept {
   auto& stream = *static_cast<std::ifstream*>(handle);
   if (origin != SEEK_SET && origin != SEEK_CUR && origin != SEEK_END) {
      return -1;
   }
   stream.clear();
   stream.seekg(static_cast<std::streamoff>(offset),
                origin == SEEK_SET ? std::ios::beg : origin == SEEK_CUR ? std::ios::cur : std::ios::end);
   return stream ? 0 : -1;
}

FLAC__int64 tellMetadata(void* handle) noexcept {
   const auto position = static_cast<std::ifstream*>(handle)->tellg();
   return position == std::streampos(-1) ? -1 : static_cast<FLAC__int64>(position);
}

int metadataEof(void* handle) noexcept {
   return static_cast<std::ifstream*>(handle)->eof() ? 1 : 0;
}

bool isCueSheetKey(std::string_view key) {
   constexpr std::string_view CUESHEET_KEY = "CUESHEET";
   if (key.size() != CUESHEET_KEY.size()) {
      return false;
   }
   for (std::size_t index = 0; index < key.size(); ++index) {
      char character = key[index];
      if (character >= 'a' && character <= 'z') {
         character = static_cast<char>(character - 'a' + 'A');
      }
      if (character != CUESHEET_KEY[index]) {
         return false;
      }
   }
   return true;
}

NativeCueSheet copyCueSheet(const FLAC__StreamMetadata_CueSheet& source) {
   NativeCueSheet result;
   result.catalogNumber = source.media_catalog_number;
   result.leadIn = source.lead_in;
   result.isCd = source.is_cd;
   for (std::uint32_t trackIndex = 0; trackIndex < source.num_tracks; ++trackIndex) {
      const auto& sourceTrack = source.tracks[trackIndex];
      CueTrack track;
      track.number = sourceTrack.number;
      track.offset = sourceTrack.offset;
      track.isrc = sourceTrack.isrc;
      track.isAudio = sourceTrack.type == 0;
      track.preEmphasis = sourceTrack.pre_emphasis != 0;
      for (std::uint32_t index = 0; index < sourceTrack.num_indices; ++index) {
         const auto& sourceIndex = sourceTrack.indices[index];
         track.indices.push_back({sourceIndex.number, sourceIndex.offset});
      }
      result.tracks.push_back(std::move(track));
   }
   return result;
}

}

FlacMetadata FlacMetadataInspector::inspect(const std::filesystem::path& path) const {
   using Chain = std::unique_ptr<FLAC__Metadata_Chain, decltype(&FLAC__metadata_chain_delete)>;
   using Iterator = std::unique_ptr<FLAC__Metadata_Iterator, decltype(&FLAC__metadata_iterator_delete)>;
   Chain chain(FLAC__metadata_chain_new(), &FLAC__metadata_chain_delete);
   Iterator iterator(FLAC__metadata_iterator_new(), &FLAC__metadata_iterator_delete);
   if (!chain || !iterator) {
      throw std::runtime_error("Unable to allocate FLAC metadata reader");
   }
   std::ifstream input(path, std::ios::binary);
   if (!input) {
      throw std::runtime_error("Unable to open FLAC metadata source");
   }
   const FLAC__IOCallbacks callbacks{&readMetadata, nullptr, &seekMetadata, &tellMetadata, &metadataEof, nullptr};
   if (!FLAC__metadata_chain_read_with_callbacks(chain.get(), &input, callbacks)) {
      const auto status = FLAC__metadata_chain_status(chain.get());
      throw std::runtime_error(std::string("Unable to read FLAC metadata: ") +
                               FLAC__Metadata_ChainStatusString[status]);
   }
   FLAC__metadata_iterator_init(iterator.get(), chain.get());
   FlacMetadata result;
   bool foundStreamInfo = false;
   do {
      const auto* block = FLAC__metadata_iterator_get_block(iterator.get());
      if (block->type == FLAC__METADATA_TYPE_STREAMINFO) {
         const auto& info = block->data.stream_info;
         result.sampleRate = info.sample_rate;
         result.channels = info.channels;
         result.bitsPerSample = info.bits_per_sample;
         result.totalSamples = info.total_samples;
         foundStreamInfo = true;
      } else if (block->type == FLAC__METADATA_TYPE_CUESHEET) {
         result.cueSheets.push_back(copyCueSheet(block->data.cue_sheet));
      } else if (block->type == FLAC__METADATA_TYPE_VORBIS_COMMENT) {
         const auto& comments = block->data.vorbis_comment;
         for (std::uint32_t index = 0; index < comments.num_comments; ++index) {
            const auto& entry = comments.comments[index];
            const std::string_view comment(reinterpret_cast<const char*>(entry.entry), entry.length);
            result.vorbisComments.emplace_back(comment);
            const auto separator = comment.find('=');
            if (separator != std::string_view::npos && isCueSheetKey(comment.substr(0, separator))) {
               result.embeddedCueSheets.emplace_back(comment.substr(separator + 1));
            }
         }
      }
   } while (FLAC__metadata_iterator_next(iterator.get()));
   if (!foundStreamInfo || result.sampleRate == 0) {
      throw std::runtime_error("Missing STREAMINFO or invalid sample rate");
   }
   return result;
}

}
