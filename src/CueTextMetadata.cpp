#include "flaccd/CueTextMetadata.h"

#include <charconv>
#include <limits>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace flaccd {
namespace {

std::string_view trim(std::string_view text) {
   const auto first = text.find_first_not_of(" \t\r");
   if (first == std::string_view::npos) {
      return {};
   }
   return text.substr(first, text.find_last_not_of(" \t\r") - first + 1);
}

std::string_view takeToken(std::string_view& text) {
   text = trim(text);
   const auto end = text.find_first_of(" \t\r");
   const auto token = text.substr(0, end);
   text = end == std::string_view::npos ? std::string_view{} : trim(text.substr(end));
   return token;
}

std::string uppercase(std::string_view text) {
   std::string result(text);
   for (auto& character : result) {
      if (character >= 'a' && character <= 'z') {
         character = static_cast<char>(character - 'a' + 'A');
      }
   }
   return result;
}

std::uint64_t parseUnsigned(std::string_view text) {
   std::uint64_t value = 0;
   if (text.empty()) {
      throw std::invalid_argument("missing unsigned integer");
   }
   const auto conversion = std::from_chars(text.data(), text.data() + text.size(), value);
   if (conversion.ec != std::errc{} || conversion.ptr != text.data() + text.size()) {
      throw std::invalid_argument("invalid unsigned integer: " + std::string(text));
   }
   return value;
}

std::string readValue(std::string_view text) {
   text = trim(text);
   if (text.empty()) {
      throw std::invalid_argument("missing value");
   }
   if (text.front() == '"') {
      const auto end = text.find('"', 1);
      if (end == std::string_view::npos || !trim(text.substr(end + 1)).empty()) {
         throw std::invalid_argument("malformed quoted value");
      }
      return std::string(text.substr(1, end - 1));
   }
   return std::string(text);
}

}

std::uint64_t parseCueTimestamp(std::string_view timestamp) {
   const auto first = timestamp.find(':');
   const auto second = first == std::string_view::npos ? first : timestamp.find(':', first + 1);
   if (first < 2 || first == std::string_view::npos || second != first + 3 ||
       timestamp.size() != second + 3) {
      throw std::invalid_argument("expected mm:ss:ff timestamp");
   }
   const auto minutes = parseUnsigned(timestamp.substr(0, first));
   const auto seconds = parseUnsigned(timestamp.substr(first + 1, 2));
   const auto frames = parseUnsigned(timestamp.substr(second + 1, 2));
   constexpr std::uint64_t SECTORS_PER_SECOND = 75;
   constexpr std::uint64_t SECTORS_PER_MINUTE = 60 * SECTORS_PER_SECOND;
   if (seconds >= 60 || frames >= SECTORS_PER_SECOND) {
      throw std::invalid_argument("timestamp seconds or frames out of range");
   }
   const auto remainder = seconds * SECTORS_PER_SECOND + frames;
   if (minutes > (std::numeric_limits<std::uint64_t>::max() - remainder) / SECTORS_PER_MINUTE) {
      throw std::invalid_argument("timestamp overflow");
   }
   return minutes * SECTORS_PER_MINUTE + remainder;
}

CueTextMetadata parseCueTextMetadata(std::string_view text) {
   if (text.starts_with("\xef\xbb\xbf")) {
      text.remove_prefix(3);
   }
   CueTextMetadata result;
   bool inTracks = false;
   bool foundFile = false;
   std::optional<std::size_t> currentTrack;
   std::unordered_set<std::string> discFields;
   std::unordered_set<std::string> trackFields;
   std::size_t lineNumber = 0;
   while (!text.empty()) {
      ++lineNumber;
      const auto end = text.find('\n');
      auto line = trim(text.substr(0, end));
      text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
      const auto command = uppercase(takeToken(line));
      try {
         if (command.empty() || command == "REM") {
            continue;
         }
         if (command == "FILE") {
            if (foundFile || inTracks) {
               throw std::invalid_argument("multiple FILE directives are unsupported");
            }
            foundFile = true;
            if (!line.empty() && line.front() == '"') {
               const auto closing = line.find('"', 1);
               if (closing == std::string_view::npos) {
                  throw std::invalid_argument("unterminated FILE name");
               }
               if (closing + 1 == line.size() || (line[closing + 1] != ' ' && line[closing + 1] != '\t')) {
                  throw std::invalid_argument("FILE name and type require whitespace separation");
               }
               result.fileName = line.substr(1, closing - 1);
               line = trim(line.substr(closing + 1));
            } else {
               result.fileName = takeToken(line);
            }
            result.fileType = uppercase(takeToken(line));
            if (result.fileName.empty() || result.fileType.empty() || !line.empty()) {
               throw std::invalid_argument("expected FILE name type");
            }
         } else if (command == "TRACK") {
            inTracks = true;
            currentTrack.reset();
            trackFields.clear();
            const auto number = parseUnsigned(takeToken(line));
            const auto type = uppercase(takeToken(line));
            if (number < 1 || number > 99 || type.empty() || !line.empty()) {
               throw std::invalid_argument("invalid TRACK number or type");
            }
            CueTrackMetadata track;
            track.number = static_cast<std::uint32_t>(number);
            track.type = type;
            result.tracks.push_back(std::move(track));
            currentTrack = result.tracks.size() - 1;
            if (!foundFile) {
               throw std::invalid_argument("TRACK requires a preceding FILE");
            }
         } else if (command == "TITLE" || command == "PERFORMER" || command == "SONGWRITER") {
            const auto value = readValue(line);
            if (inTracks && !currentTrack) {
               throw std::invalid_argument("descriptive field outside a valid TRACK");
            }
            if (!inTracks && foundFile) {
               throw std::invalid_argument("disc descriptive fields must precede FILE");
            }
            if (currentTrack && !result.tracks[*currentTrack].indices.empty()) {
               throw std::invalid_argument("track descriptive fields must precede INDEX");
            }
            auto& fields = currentTrack ? trackFields : discFields;
            if (!fields.emplace(command).second) {
               throw std::invalid_argument("duplicate descriptive field");
            }
            if (command != "SONGWRITER") {
               if (currentTrack) {
                  auto& track = result.tracks[*currentTrack];
                  (command == "TITLE" ? track.title : track.performer) = value;
               } else {
                  (command == "TITLE" ? result.title : result.performer) = value;
               }
            }
         } else if (command == "CATALOG") {
            if (foundFile || inTracks || !result.catalogNumber.empty()) {
               throw std::invalid_argument("misplaced or duplicate CATALOG");
            }
            result.catalogNumber = readValue(line);
            if (result.catalogNumber.empty()) {
               throw std::invalid_argument("empty CATALOG");
            }
         } else if (command == "INDEX" || command == "PREGAP" || command == "POSTGAP" ||
                    command == "FLAGS" || command == "ISRC") {
            if (!currentTrack) {
               throw std::invalid_argument("track directive outside a valid TRACK");
            }
            auto& track = result.tracks[*currentTrack];
            if (command != "INDEX" && !trackFields.emplace(command).second) {
               throw std::invalid_argument("duplicate track directive");
            }
            if (command == "INDEX") {
               const auto number = parseUnsigned(takeToken(line));
               const auto sector = parseCueTimestamp(takeToken(line));
               if (number > 99 || !line.empty() || trackFields.contains("POSTGAP")) {
                  throw std::invalid_argument("invalid or misplaced INDEX");
               }
               track.indices.push_back({static_cast<std::uint32_t>(number), sector});
            } else if (command == "PREGAP" || command == "POSTGAP") {
               const auto duration = parseCueTimestamp(takeToken(line));
               if (!line.empty() || (command == "PREGAP" && !track.indices.empty()) ||
                   (command == "POSTGAP" && track.indices.empty())) {
                  throw std::invalid_argument("misplaced gap directive");
               }
               (command == "PREGAP" ? track.pregap : track.postgap) = duration;
            } else if (command == "ISRC") {
               if (!track.indices.empty()) {
                  throw std::invalid_argument("ISRC must precede INDEX");
               }
               track.isrc = readValue(line);
               if (track.isrc.empty()) {
                  throw std::invalid_argument("empty ISRC");
               }
            } else {
               if (line.empty() || !track.indices.empty()) {
                  throw std::invalid_argument("missing or misplaced FLAGS");
               }
               std::unordered_set<std::string> flags;
               while (!line.empty()) {
                  const auto flag = uppercase(takeToken(line));
                  if (!flags.emplace(flag).second) {
                     throw std::invalid_argument("duplicate flag");
                  }
                  if (flag == "PRE") {
                     track.preEmphasis = true;
                  } else if (flag == "DCP") {
                     track.digitalCopyPermitted = true;
                  } else {
                     throw std::invalid_argument("unsupported flag: " + std::string(flag));
                  }
               }
            }
         } else {
            throw std::invalid_argument("unsupported CUE command: " + std::string(command));
         }
      } catch (const std::invalid_argument& error) {
         result.errors.push_back("line " + std::to_string(lineNumber) + ": " + error.what());
      }
   }
   if (!foundFile) {
      result.errors.push_back("missing FILE directive");
   }
   return result;
}

}
