#include "flaccd/mount/WindowsImgDriveHost.h"
#include "ProcessArgumentDiagnostics.h"

#include <windows.h>

#include <algorithm>
#include <fstream>
#include <iostream>
#include <random>
#include <stdexcept>

namespace {

void require(bool condition, const char* message) {
   if (!condition) {
      throw std::runtime_error(message);
   }
}

std::u16string expectedUtf16(std::string_view value) {
   if (value.empty()) {
      return {};
   }
   const auto length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                                           static_cast<int>(value.size()), nullptr, 0);
   require(length > 0, "test input is not valid UTF-8");
   std::wstring wide(length, L'\0');
   require(MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
                               wide.data(), length) == length, "expected UTF-16 conversion failed");
   std::u16string output;
   for (const auto unit : wide) {
      output += static_cast<char16_t>(unit);
   }
   return output;
}

std::string actualUtf8(std::u16string_view value) {
   if (value.empty()) {
      return {};
   }
   std::wstring wide;
   for (const auto unit : value) {
      wide += static_cast<wchar_t>(unit);
   }
   const auto length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(),
                                           static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
   require(length > 0, "child argv contains invalid UTF-16");
   std::string output(length, '\0');
   require(WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), static_cast<int>(wide.size()),
                               output.data(), length, nullptr, nullptr) == length, "actual UTF-8 conversion failed");
   return output;
}

void checkRoundTrip(flaccd::WindowsImgDriveHost& host,
                    const std::filesystem::path& probe,
                    std::string_view name,
                    const std::vector<std::string>& values) {
   std::string expectedBytes;
   flaccd::test::ProcessArguments expectedArguments;
   for (const auto& value : values) {
      expectedBytes += value + '\n';
      expectedArguments.push_back(expectedUtf16(value));
   }
   const auto echoed = host.run(probe, values, std::chrono::seconds(5));
   auto dumpArguments = values;
   dumpArguments.insert(dumpArguments.begin(), "--dump-argv");
   const auto dumped = host.run(probe, dumpArguments, std::chrono::seconds(5));
   std::cerr << "Argument case: " << name << "\n"
             << "  echo exit=" << echoed.exitCode << " timeout=" << echoed.timedOut
             << "; argv dump exit=" << dumped.exitCode << " timeout=" << dumped.timedOut << '\n'
             << "  expected payload bytes=" << expectedBytes.size() << ' '
             << flaccd::test::escapeBytes(expectedBytes) << '\n'
             << "  actual payload bytes=" << echoed.diagnostics.size() << ' '
             << flaccd::test::escapeBytes(echoed.diagnostics) << '\n';
   flaccd::test::ProcessArguments received;
   std::string decodeError;
   try {
      received = flaccd::test::decodeArguments(dumped.diagnostics);
   } catch (const std::exception& error) {
      decodeError = error.what();
      std::cerr << "  raw argv decode error: " << decodeError << "\n  dump bytes="
                << flaccd::test::escapeBytes(dumped.diagnostics) << '\n';
   }
   std::cerr << "  expected argument count=" << values.size() << "; actual count=";
   if (decodeError.empty()) {
      std::cerr << received.size() << '\n';
   } else {
      std::cerr << "unavailable\n";
   }
   for (std::size_t position = 0; position < std::max(values.size(), received.size()); ++position) {
      std::cerr << "  argument[" << position << "] (echo argv[" << position + 1
                << "], dump argv[" << position + 2 << "])\n";
      if (position < values.size()) {
         std::cerr << "    expected UTF-8 bytes=" << values[position].size() << ' '
                   << flaccd::test::escapeBytes(values[position]) << "\n    expected UTF-16 units="
                   << expectedArguments[position].size() << ' '
                   << flaccd::test::escapeUtf16(expectedArguments[position]) << '\n';
      } else {
         std::cerr << "    expected: <no argument>\n";
      }
      if (position < received.size()) {
         std::cerr << "    actual UTF-16 units=" << received[position].size() << ' '
                   << flaccd::test::escapeUtf16(received[position]) << '\n';
         try {
            const auto utf8 = actualUtf8(received[position]);
            std::cerr << "    actual UTF-8 bytes=" << utf8.size() << ' ' << flaccd::test::escapeBytes(utf8) << '\n';
         } catch (const std::exception& error) {
            std::cerr << "    actual UTF-8 unavailable: " << error.what() << '\n';
         }
      } else {
         std::cerr << "    actual: <missing or unavailable>\n";
      }
   }
   // Separate assertions identify process errors, CRT argv parsing, UTF-8 echo
   // encoding, and newline/byte transport failures rather than conflating them.
   require(!echoed.timedOut, "argv echo process timed out");
   require(echoed.exitCode == 0, "argv echo process returned nonzero");
   require(!dumped.timedOut, "raw argv dump process timed out");
   require(dumped.exitCode == 0, "raw argv dump process returned nonzero");
   require(decodeError.empty(), "raw argv protocol decode failed");
   require(received.size() == expectedArguments.size(), "Windows argv argument count mismatch");
   for (std::size_t position = 0; position < expectedArguments.size(); ++position) {
      if (received[position] != expectedArguments[position]) {
         throw std::runtime_error("Windows argv UTF-16 mismatch at argument[" + std::to_string(position) + "]");
      }
      if (actualUtf8(received[position]) != values[position]) {
         throw std::runtime_error("Windows argv UTF-8 mismatch at argument[" + std::to_string(position) + "]");
      }
   }
   require(echoed.diagnostics == expectedBytes, "argv echo byte transport mismatch (check escaped CR/LF above)");
}

}

int wmain(int argumentCount, wchar_t* arguments[]) {
   std::filesystem::path directory;
   try {
      require(argumentCount == 2, "expected probe executable");
      flaccd::WindowsImgDriveHost host;
      const std::filesystem::path probe(arguments[1]);
      const std::vector<std::string> values{"space name", "音楽 é", "quote\"test", "ends\\", "&;|<>^", ""};
      checkRoundTrip(host, probe, "original regression", values);
      const std::vector<std::string> filenames{"", "", "C:\\Music\\Zelda.flac",
                                               "C:\\Music with spaces\\音楽 é.flac",
                                               "\\\\server\\Music share\\Zelda.flac",
                                               "/literal/not-a-path", "--path=/literal/not-a-path",
                                               "tab\tvalue", "line\nbreak", "carriage\rreturn",
                                               "CRLF\r\nvalue", "emoji \xf0\x9f\x8e\xb5.flac", ""};
      checkRoundTrip(host, probe, "filenames, control characters, and supplementary Unicode", filenames);
      std::vector<std::string> backslashes{"first", "", "\"", "\"\"", "last"};
      for (std::size_t count = 0; count <= 8; ++count) {
         backslashes.push_back("prefix" + std::string(count, '\\') + "\"suffix");
         backslashes.push_back("with spaces " + std::string(count, '\\'));
         backslashes.push_back(std::string(count, '\\') + "\"");
      }
      checkRoundTrip(host, probe, "backslash/quote parity and empty arguments", backslashes);
      const std::vector<std::string> failure{"fail"};
      const auto failed = host.run(probe, failure, std::chrono::seconds(5));
      require(failed.exitCode == 7 && failed.diagnostics == "expected failure", "process diagnostics/exit code");
      const std::vector<std::string> sleep{"sleep"};
      require(host.run(probe, sleep, std::chrono::milliseconds(50)).timedOut, "process timeout");
      const std::vector<std::string> flood{"flood"};
      const auto captured = host.run(probe, flood, std::chrono::seconds(5));
      require(captured.exitCode == 0 && !captured.timedOut && captured.diagnostics.size() == 65536,
              "bounded capture must keep draining a noisy child");
      std::random_device generator;
      directory = std::filesystem::temp_directory_path() / ("flaccd-process-" + std::to_string(generator()));
      require(std::filesystem::create_directory(directory), "create test directory");
      const auto invalid = directory / "invalid.exe";
      {
         std::ofstream output(invalid, std::ios::binary);
         output << "MZinvalid PE file";
      }
      for (const auto& path : {invalid, directory / "missing.exe"}) {
         bool rejected = false;
         try {
            host.run(path, {}, std::chrono::seconds(1));
         } catch (const std::runtime_error&) {
            rejected = true;
         }
         require(rejected, "CreateProcessW must reject missing/invalid executables");
      }
      std::filesystem::remove(invalid);
      std::filesystem::remove(directory);
      std::cout << "Windows process adapter tests passed\n";
   } catch (const std::exception& error) {
      std::cerr << error.what() << '\n';
      // Retain unexpected temporary contents rather than deleting arbitrary files.
      return 1;
   }
   return 0;
}
