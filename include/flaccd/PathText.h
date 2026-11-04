#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace flaccd {

inline std::string pathToUtf8(const std::filesystem::path& path) {
   const auto text = path.u8string();
   return {reinterpret_cast<const char*>(text.data()), text.size()};
}

inline std::filesystem::path pathFromUtf8(std::string_view text) {
   return std::filesystem::path(std::u8string(text.begin(), text.end()));
}

}
