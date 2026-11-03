#include "flaccd/mount/WindowsRestoreStartup.h"

#include <windows.h>

#include <stdexcept>
#include <system_error>
#include <utility>

namespace flaccd {
namespace {

constexpr wchar_t VALUE_NAME[] = L"FlacCD.Restore";

struct RegistryKey {
   HKEY value = nullptr;
   RegistryKey() = default;
   RegistryKey(const RegistryKey&) = delete;
   RegistryKey& operator=(const RegistryKey&) = delete;
   ~RegistryKey() {
      if (value) {
         RegCloseKey(value);
      }
   }
};

void check(LSTATUS result, const char* operation) {
   if (result != ERROR_SUCCESS) {
      throw std::system_error(static_cast<int>(result), std::system_category(), operation);
   }
}

}

WindowsRestoreStartup::WindowsRestoreStartup(std::wstring registryRoot) : root(std::move(registryRoot)) {
}

std::optional<std::wstring> WindowsRestoreStartup::read() {
   RegistryKey key;
   const auto opened = RegOpenKeyExW(HKEY_CURRENT_USER, root.c_str(), 0, KEY_QUERY_VALUE, &key.value);
   if (opened == ERROR_FILE_NOT_FOUND) {
      return {};
   }
   check(opened, "open per-user restore startup");
   wchar_t value[32768];
   DWORD type = 0;
   DWORD size = sizeof(value);
   const auto result = RegQueryValueExW(key.value, VALUE_NAME, nullptr, &type, reinterpret_cast<BYTE*>(value), &size);
   if (result == ERROR_FILE_NOT_FOUND) {
      return {};
   }
   check(result, "read per-user restore startup");
   if (type != REG_SZ || size < sizeof(wchar_t) || size % sizeof(wchar_t) != 0 ||
       value[size / sizeof(wchar_t) - 1] != L'\0') {
      throw std::runtime_error("foreign/invalid restore startup value; preserved");
   }
   std::wstring command(value, size / sizeof(wchar_t) - 1);
   if (command.find(L'\0') != std::wstring::npos) {
      throw std::runtime_error("embedded NUL in restore startup value; preserved");
   }
   return command;
}

void WindowsRestoreStartup::write(const std::wstring& command) {
   RegistryKey key;
   check(RegCreateKeyExW(HKEY_CURRENT_USER, root.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key.value, nullptr),
         "open per-user restore startup for registration");
   check(RegSetValueExW(key.value, VALUE_NAME, 0, REG_SZ, reinterpret_cast<const BYTE*>(command.c_str()),
                        static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t))), "register per-user restoration");
}

void WindowsRestoreStartup::remove() {
   RegistryKey key;
   const auto opened = RegOpenKeyExW(HKEY_CURRENT_USER, root.c_str(), 0, KEY_SET_VALUE, &key.value);
   if (opened == ERROR_FILE_NOT_FOUND) {
      return;
   }
   check(opened, "open per-user restore startup for removal");
   const auto removed = RegDeleteValueW(key.value, VALUE_NAME);
   if (removed != ERROR_FILE_NOT_FOUND) {
      check(removed, "remove per-user restoration");
   }
}

}
