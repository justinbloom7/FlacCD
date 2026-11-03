#include "ProcessArgumentDiagnostics.h"

#include <windows.h>
#include <fcntl.h>
#include <io.h>

#include <chrono>
#include <cstdio>
#include <iostream>
#include <thread>

namespace {

std::string toUtf8(std::wstring_view value) {
   if (value.empty()) {
      return {};
   }
   const auto length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                                           static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
   if (length == 0) {
      throw std::runtime_error("probe: invalid UTF-16 argument");
   }
   std::string output(length, '\0');
   if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
                           output.data(), length, nullptr, nullptr) != length) {
      throw std::runtime_error("probe: UTF-8 conversion failed");
   }
   return output;
}

}

int wmain(int argumentCount, wchar_t* arguments[]) {
   // UCRT stdout/stderr default to text mode, even for inherited pipes. Without
   // binary mode, each LF becomes CRLF and byte-exact transport checks fail.
   // Set this before any stream I/O; do not alter production capture bytes.
   if (_setmode(_fileno(stdout), _O_BINARY) == -1 || _setmode(_fileno(stderr), _O_BINARY) == -1) {
      std::fputs("probe: cannot set binary output mode", stderr);
      return 9;
   }
   try {
      if (argumentCount >= 2 && std::wstring_view(arguments[1]) == L"sleep") {
         std::this_thread::sleep_for(std::chrono::seconds(5));
         return 0;
      }
      if (argumentCount >= 2 && std::wstring_view(arguments[1]) == L"fail") {
         std::cerr << "expected failure";
         return 7;
      }
      if (argumentCount >= 2 && std::wstring_view(arguments[1]) == L"flood") {
         std::cout << std::string(512 * 1024, 'x');
         return 0;
      }
      if (argumentCount >= 2 && std::wstring_view(arguments[1]) == L"--dump-argv") {
         static_assert(sizeof(wchar_t) == sizeof(char16_t));
         flaccd::test::ProcessArguments received;
         for (int position = 2; position < argumentCount; ++position) {
            std::u16string argument;
            for (const wchar_t unit : std::wstring_view(arguments[position])) {
               argument += static_cast<char16_t>(unit);
            }
            received.push_back(std::move(argument));
         }
         std::cout << flaccd::test::encodeArguments(received);
      } else {
         for (int position = 1; position < argumentCount; ++position) {
            // Independent of the production filesystem-based UTF conversion.
            std::cout << toUtf8(arguments[position]) << '\n';
         }
      }
      std::cout.flush();
      return std::cout ? 0 : 8;
   } catch (const std::exception& error) {
      std::cerr << error.what();
      return 8;
   }
}
