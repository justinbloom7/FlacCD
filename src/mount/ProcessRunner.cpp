#include "flaccd/mount/ProcessRunner.h"

#include <stdexcept>

namespace flaccd {

std::wstring quoteWindowsArgument(std::wstring_view argument) {
   if (argument.find(L'\0') != std::wstring_view::npos) {
      throw std::invalid_argument("process argument contains NUL");
   }
   std::wstring result = L"\"";
   std::size_t backslashes = 0;
   for (const auto character : argument) {
      if (character == L'\\') {
         ++backslashes;
         continue;
      }
      result.append(character == L'"' ? backslashes * 2 + 1 : backslashes, L'\\');
      backslashes = 0;
      result += character;
   }
   result.append(backslashes * 2, L'\\');
   result += L'"';
   return result;
}

}
