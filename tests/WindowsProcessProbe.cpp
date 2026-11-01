#include "flaccd/PathText.h"

#include <chrono>
#include <iostream>
#include <thread>

int wmain(int argumentCount, wchar_t* arguments[]) {
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
   for (int position = 1; position < argumentCount; ++position) {
      std::cout << flaccd::pathToUtf8(std::filesystem::path(arguments[position])) << '\n';
   }
   return 0;
}
