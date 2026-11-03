#include "ProcessArgumentDiagnostics.h"

#include <iostream>

namespace {

void require(bool condition, const char* message) {
   if (!condition) {
      throw std::runtime_error(message);
   }
}

template<typename Operation>
void expectError(Operation operation) {
   try {
      operation();
   } catch (const std::runtime_error&) {
      return;
   }
   throw std::runtime_error("expected malformed argv dump rejection");
}

}

int main() {
   try {
      const flaccd::test::ProcessArguments arguments{u"", u"", u"space name", u"quote\"test", u"ends\\",
                                                     u"\u97f3\u697d \u00e9", u"\U0001f3b5.flac", u"&;|<>^",
                                                     u"line\nbreak", u"CRLF\r\nvalue", u"tab\tvalue", u""};
      const auto encoded = flaccd::test::encodeArguments(arguments);
      require(flaccd::test::decodeArguments(encoded) == arguments, "raw UTF-16 argument dump round trip");
      require(flaccd::test::decodeArguments(flaccd::test::encodeArguments({})).empty(), "empty argv vector");
      require(flaccd::test::escapeUtf16(arguments[5]) == "\"\\u97f3\\u697d \\u00e9\"", "Unicode escaping");
      require(flaccd::test::escapeUtf16(arguments[6]) == "\"\\ud83c\\udfb5.flac\"", "surrogate pair escaping");
      require(flaccd::test::escapeBytes("a\r\nb\t\\\"") == "\"a\\r\\nb\\t\\\\\\\"\"", "CR/LF/quote diagnostics");
      std::string translated;
      for (const auto character : encoded) {
         if (character == '\n') {
            translated += '\r';
         }
         translated += character;
      }
      expectError([&] { flaccd::test::decodeArguments(translated); });
      expectError([&] { flaccd::test::decodeArguments(encoded.substr(0, encoded.size() - 1)); });
      expectError([&] { flaccd::test::decodeArguments(encoded + "extra"); });
      expectError([&] { flaccd::test::decodeArguments("FLACCD_ARGV_V1\n1\n2:0061\n"); });
      expectError([&] { flaccd::test::decodeArguments("FLACCD_ARGV_V1\n1\n1:zzzz\n"); });
      expectError([&] { flaccd::test::decodeArguments("FLACCD_ARGV_V1\n999999999999999999999999\n"); });
      std::cout << "Argument diagnostic/framing tests passed, including rejection of CRLF translation\n";
   } catch (const std::exception& error) {
      std::cerr << error.what() << '\n';
      return 1;
   }
   return 0;
}
