#include "flaccd/shell/WindowsShellRegistry.h"

#include <windows.h>
#include <sddl.h>
#include <shlobj.h>

#include <stdexcept>
#include <system_error>
#include <vector>

namespace flaccd {
namespace {

constexpr wchar_t VERB[] = L"FlacCD.MountAudioCD";

void check(LSTATUS result, const char* operation) {
   if (result != ERROR_SUCCESS) {
      throw std::system_error(static_cast<int>(result), std::system_category(), operation);
   }
}

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

struct Handle {
   HANDLE value = nullptr;
   ~Handle() {
      if (value) {
         CloseHandle(value);
      }
   }
};

struct SidText {
   wchar_t* value = nullptr;
   ~SidText() {
      LocalFree(value);
   }
};

void readValues(HKEY key, const std::wstring& child, ShellValues& values) {
   DWORD count = 0;
   DWORD nameLength = 0;
   DWORD dataLength = 0;
   check(RegQueryInfoKeyW(key, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                         &count, &nameLength, &dataLength, nullptr, nullptr), "Query Explorer values");
   if (nameLength > 32768 || dataLength > 131072 || count > 32) {
      throw std::runtime_error("Unexpected Explorer registration size; preserved");
   }
   std::vector<wchar_t> name(nameLength + 1);
   std::vector<wchar_t> data(dataLength / sizeof(wchar_t) + 1);
   for (DWORD index = 0; index < count; ++index) {
      DWORD characters = static_cast<DWORD>(name.size());
      DWORD bytes = static_cast<DWORD>(data.size() * sizeof(wchar_t));
      DWORD type = 0;
      check(RegEnumValueW(key, index, name.data(), &characters, nullptr, &type,
                          reinterpret_cast<BYTE*>(data.data()), &bytes), "Read Explorer value");
      if (type != REG_SZ || bytes < sizeof(wchar_t) || bytes % sizeof(wchar_t) != 0 ||
          data[bytes / sizeof(wchar_t) - 1] != L'\0') {
         throw std::runtime_error("Unexpected Explorer value type/encoding; preserved");
      }
      const std::wstring text(data.data(), bytes / sizeof(wchar_t) - 1);
      if (text.find(L'\0') != std::wstring::npos) {
         throw std::runtime_error("Unexpected embedded NUL in Explorer registration; preserved");
      }
      values[{child, std::wstring(name.data(), characters)}] = text;
   }
}

void verifyChildren(HKEY key, bool verb) {
   DWORD count = 0;
   check(RegQueryInfoKeyW(key, nullptr, nullptr, nullptr, &count, nullptr, nullptr,
                         nullptr, nullptr, nullptr, nullptr, nullptr), "Query Explorer subkeys");
   if (count > (verb ? 1U : 0U)) {
      throw std::runtime_error("Unexpected Explorer registration subkeys; preserved");
   }
   if (count == 1) {
      wchar_t name[256];
      DWORD length = 256;
      check(RegEnumKeyExW(key, 0, name, &length, nullptr, nullptr, nullptr, nullptr), "Read Explorer subkey");
      if (std::wstring_view(name, length) != L"command") {
         throw std::runtime_error("Unexpected Explorer registration subkey; preserved");
      }
   }
}

}

struct WindowsShellRegistry::State {
   std::wstring path;
   HANDLE mutex = nullptr;
   bool locked = false;
   ~State() {
      if (locked) {
         ReleaseMutex(mutex);
      }
      if (mutex) {
         CloseHandle(mutex);
      }
   }
};

WindowsShellRegistry::WindowsShellRegistry(std::wstring root) : state(std::make_unique<State>()) {
   state->path = std::move(root) + L"\\" + VERB;
   Handle token;
   if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token.value)) {
      check(GetLastError(), "Get registry lock identity");
   }
   DWORD bytes = 0;
   GetTokenInformation(token.value, TokenUser, nullptr, 0, &bytes);
   if (bytes == 0) {
      check(GetLastError(), "Size registry lock identity");
   }
   std::vector<BYTE> buffer(bytes);
   const BOOL success = GetTokenInformation(token.value, TokenUser, buffer.data(), bytes, &bytes);
   const DWORD error = success ? ERROR_SUCCESS : GetLastError();
   check(error, "Read registry lock identity");
   SidText sid;
   if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid, &sid.value)) {
      check(GetLastError(), "Format registry lock identity");
   }
   const std::wstring name = std::wstring(L"Local\\FlacCD.Shell.v1.") + sid.value;
   state->mutex = CreateMutexW(nullptr, FALSE, name.c_str());
   if (!state->mutex) {
      check(GetLastError(), "Create Explorer registration lock");
   }
   const DWORD wait = WaitForSingleObject(state->mutex, 10000);
   if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED) {
      throw std::runtime_error("Timed out acquiring Explorer registration lock");
   }
   state->locked = true;
}

WindowsShellRegistry::~WindowsShellRegistry() = default;

std::optional<ShellValues> WindowsShellRegistry::read() {
   RegistryKey key;
   const auto result = RegOpenKeyExW(HKEY_CURRENT_USER, state->path.c_str(), 0, KEY_READ, &key.value);
   if (result == ERROR_FILE_NOT_FOUND) {
      return std::nullopt;
   }
   check(result, "Open Explorer registration");
   verifyChildren(key.value, true);
   ShellValues values;
   readValues(key.value, L"", values);
   RegistryKey command;
   const auto commandResult = RegOpenKeyExW(key.value, L"command", 0, KEY_READ, &command.value);
   if (commandResult != ERROR_FILE_NOT_FOUND) {
      check(commandResult, "Open Explorer command");
      verifyChildren(command.value, false);
      readValues(command.value, L"command", values);
   }
   return values;
}

void WindowsShellRegistry::create(const ShellValues& values) {
   RegistryKey key;
   DWORD disposition = 0;
   check(RegCreateKeyExW(HKEY_CURRENT_USER, state->path.c_str(), 0, nullptr, 0,
                        KEY_READ | KEY_WRITE, nullptr, &key.value, &disposition), "Create Explorer registration");
   if (disposition != REG_CREATED_NEW_KEY) {
      throw std::runtime_error("Explorer registration appeared concurrently; preserved; retry shell status");
   }
   try {
      for (const auto& [location, text] : values) {
         RegistryKey child;
         HKEY destination = key.value;
         if (!location.first.empty()) {
            check(RegCreateKeyExW(key.value, location.first.c_str(), 0, nullptr, 0,
                                 KEY_WRITE, nullptr, &child.value, nullptr), "Create Explorer command");
            destination = child.value;
         }
         check(RegSetValueExW(destination, location.second.c_str(), 0, REG_SZ,
                              reinterpret_cast<const BYTE*>(text.c_str()),
                              static_cast<DWORD>((text.size() + 1) * sizeof(wchar_t))), "Write Explorer registration");
      }
   } catch (...) {
      // Roll back only unchanged values from this attempt. An unexpected edit
      // or subkey is preserved. Never recursively delete a registration tree.
      bool unchanged = false;
      try {
         const auto partial = read();
         unchanged = partial.has_value();
         if (partial) {
            for (const auto& [location, text] : *partial) {
               const auto expected = values.find(location);
               if (expected == values.end() || expected->second != text) {
                  unchanged = false;
               }
            }
         }
      } catch (const std::exception&) {
         unchanged = false;
      }
      if (unchanged) {
         RegDeleteKeyW(key.value, L"command");
         RegDeleteKeyW(HKEY_CURRENT_USER, state->path.c_str());
      }
      throw;
   }
   SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
}

void WindowsShellRegistry::remove(const ShellValues& expected) {
   if (read() != std::optional<ShellValues>(expected)) {
      throw std::runtime_error("Explorer registration changed during removal; preserved");
   }
   RegistryKey key;
   check(RegOpenKeyExW(HKEY_CURRENT_USER, state->path.c_str(), 0, KEY_WRITE, &key.value), "Open owned Explorer registration");
   check(RegDeleteKeyW(key.value, L"command"), "Remove owned Explorer command");
   check(RegDeleteKeyW(HKEY_CURRENT_USER, state->path.c_str()), "Remove owned Explorer registration");
   SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
}

std::wstring shellExecutablePath() {
   std::vector<wchar_t> buffer(32768);
   const auto length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
   if (length == 0 || length >= buffer.size()) {
      throw std::runtime_error("Cannot determine absolute flaccd.exe path for Explorer registration");
   }
   return std::wstring(buffer.data(), length);
}

}
