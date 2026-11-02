#pragma once

#include <charconv>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace flaccd::test {

using ProcessArguments = std::vector<std::u16string>;
constexpr std::string_view ARGUMENT_MAGIC = "FLACCD_ARGV_V1\n";
constexpr std::string_view HEX_DIGITS = "0123456789abcdef";

inline void appendHex(std::string& output, std::uint32_t value, unsigned digits) {
   for (unsigned position = digits; position > 0; --position) {
      output += HEX_DIGITS[(value >> ((position - 1) * 4)) & 15];
   }
}

inline std::string escapeBytes(std::string_view value) {
   std::string output = "\"";
   for (const unsigned char character : value) {
      if (character == '\\' || character == '"') {
         output += '\\';
         output += static_cast<char>(character);
      } else if (character == '\n') {
         output += "\\n";
      } else if (character == '\r') {
         output += "\\r";
      } else if (character == '\t') {
         output += "\\t";
      } else if (character >= 32 && character < 127) {
         output += static_cast<char>(character);
      } else {
         output += "\\x";
         appendHex(output, character, 2);
      }
   }
   return output + '"';
}

inline std::string escapeUtf16(std::u16string_view value) {
   std::string output = "\"";
   for (const auto character : value) {
      if (character == u'\\' || character == u'"') {
         output += '\\';
         output += static_cast<char>(character);
      } else if (character == u'\n') {
         output += "\\n";
      } else if (character == u'\r') {
         output += "\\r";
      } else if (character == u'\t') {
         output += "\\t";
      } else if (character >= 32 && character < 127) {
         output += static_cast<char>(character);
      } else {
         output += "\\u";
         appendHex(output, character, 4);
      }
   }
   return output + '"';
}

// ASCII length-framed hex of raw UTF-16 argv units: independent of console
// encoding, filesystem::path conversion, delimiters inside args, and locale.
inline std::string encodeArguments(std::span<const std::u16string> arguments) {
   std::string output(ARGUMENT_MAGIC);
   output += std::to_string(arguments.size()) + '\n';
   for (const auto& argument : arguments) {
      output += std::to_string(argument.size()) + ':';
      for (const auto unit : argument) {
         appendHex(output, unit, 4);
      }
      output += '\n';
   }
   return output;
}

inline ProcessArguments decodeArguments(std::string_view input) {
   if (!input.starts_with(ARGUMENT_MAGIC)) {
      throw std::runtime_error("missing raw argv protocol header (check probe rebuild/output mode)");
   }
   input.remove_prefix(ARGUMENT_MAGIC.size());
   const auto takeLine = [&]() {
      const auto end = input.find('\n');
      if (end == std::string_view::npos) {
         throw std::runtime_error("truncated raw argv record");
      }
      const auto line = input.substr(0, end);
      input.remove_prefix(end + 1);
      return line;
   };
   const auto parseLength = [](std::string_view text) {
      std::size_t result = 0;
      const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
      if (text.empty() || parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) {
         throw std::runtime_error("invalid raw argv length");
      }
      return result;
   };
   const auto count = parseLength(takeLine());
   if (count > input.size()) {
      throw std::runtime_error("impossible raw argv count");
   }
   ProcessArguments arguments;
   for (std::size_t position = 0; position < count; ++position) {
      const auto line = takeLine();
      const auto separator = line.find(':');
      if (separator == std::string_view::npos) {
         throw std::runtime_error("missing raw argv length separator");
      }
      const auto length = parseLength(line.substr(0, separator));
      const auto data = line.substr(separator + 1);
      if (data.size() % 4 != 0 || length != data.size() / 4) {
         throw std::runtime_error("raw argv length does not match UTF-16 hex data");
      }
      std::u16string argument;
      for (std::size_t unit = 0; unit < length; ++unit) {
         std::uint16_t value = 0;
         const auto text = data.substr(unit * 4, 4);
         const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value, 16);
         if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) {
            throw std::runtime_error("invalid UTF-16 hex unit");
         }
         argument += static_cast<char16_t>(value);
      }
      arguments.push_back(std::move(argument));
   }
   if (!input.empty()) {
      throw std::runtime_error("unexpected trailing raw argv data");
   }
   return arguments;
}

}
