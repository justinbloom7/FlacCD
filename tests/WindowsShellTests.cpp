#include "flaccd/shell/WindowsShellRegistry.h"

#include <windows.h>
#include <objbase.h>
#include <shellapi.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {

void require(bool condition, const char* message) {
   if (!condition) {
      throw std::runtime_error(std::string(message) + " (Win32=" + std::to_string(GetLastError()) + ")");
   }
}

struct Fixture {
   std::wstring root = L"Software\\FlacCD\\ShellTests\\" + std::to_wstring(GetCurrentProcessId()) +
                       L"-" + std::to_wstring(GetTickCount64());
   std::filesystem::path directory = std::filesystem::temp_directory_path() /
      (L"FlacCD shell test " + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
   bool createdDirectory = false;
   ~Fixture() {
      // This isolated test namespace is never the real file association.
      RegDeleteTreeW(HKEY_CURRENT_USER, root.c_str());
      std::error_code error;
      if (createdDirectory) {
         std::filesystem::remove_all(directory, error);
      }
   }
};

void roundTrip(const Fixture& fixture, const std::filesystem::path& probe,
               const std::wstring& folder) {
   const auto directory = fixture.directory / folder;
   std::filesystem::create_directories(directory);
   const auto executable = directory / L"flaccd.exe";
   std::filesystem::copy_file(probe, executable);
   const auto file = directory / L"album \u97f3\u697d & ^ ! %1 %L %PATH% (disc);'.flac";
   std::ofstream(file).put('\0');
   flaccd::WindowsShellRegistry registry(fixture.root + L"\\shell");
   flaccd::runShellCommand(registry, "install", executable.wstring());
   const auto original = registry.read();
   flaccd::runShellCommand(registry, "install", executable.wstring());
   require(registry.read() == original, "Native install not idempotent");
   HKEY verbKey = nullptr;
   require(RegOpenKeyExW(HKEY_CURRENT_USER, (fixture.root + L"\\shell\\FlacCD.MountAudioCD").c_str(),
                         0, KEY_SET_VALUE, &verbKey) == ERROR_SUCCESS, "Open test verb");
   const wchar_t foreign[] = L"do not remove";
   require(RegSetValueExW(verbKey, L"Unrelated", 0, REG_SZ, reinterpret_cast<const BYTE*>(foreign),
                          sizeof(foreign)) == ERROR_SUCCESS, "Set conflict value");
   bool rejected = false;
   try {
      flaccd::runShellCommand(registry, "uninstall", executable.wstring());
   } catch (const std::runtime_error&) {
      rejected = true;
   }
   require(rejected && registry.read()->size() == original->size() + 1, "Native modified entry not preserved");
   require(RegDeleteValueW(verbKey, L"Unrelated") == ERROR_SUCCESS, "Remove test conflict value");
   RegCloseKey(verbKey);
   HKEY classKey = nullptr;
   require(RegOpenKeyExW(HKEY_CURRENT_USER, fixture.root.c_str(), 0, KEY_READ, &classKey) == ERROR_SUCCESS,
           "Open isolated shell class");
   SHELLEXECUTEINFOW request{};
   request.cbSize = sizeof(request);
   request.fMask = SEE_MASK_CLASSKEY | SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI | SEE_MASK_NOASYNC;
   request.hkeyClass = classKey;
   request.lpVerb = L"FlacCD.MountAudioCD";
   request.lpFile = file.c_str();
   request.nShow = SW_HIDE;
   const BOOL launched = ShellExecuteExW(&request);
   const DWORD launchError = GetLastError();
   RegCloseKey(classKey);
   SetLastError(launchError);
   require(launched && request.hProcess, "ShellExecute isolated registration");
   const DWORD wait = WaitForSingleObject(request.hProcess, 10000);
   DWORD exitCode = 0;
   const BOOL queried = GetExitCodeProcess(request.hProcess, &exitCode);
   CloseHandle(request.hProcess);
   require(wait == WAIT_OBJECT_0 && queried && exitCode == 0, "Shell argument probe failed");
   std::ifstream input(std::filesystem::path(executable.wstring() + L".args"), std::ios::binary);
   const std::vector<char> actual((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
   const auto expected = file.wstring();
   const std::string bytes(reinterpret_cast<const char*>(expected.data()), expected.size() * sizeof(wchar_t));
   require(std::string(actual.begin(), actual.end()) == bytes, "Explorer filename UTF-16 round trip differs");
   flaccd::runShellCommand(registry, "uninstall", executable.wstring());
   flaccd::runShellCommand(registry, "uninstall", executable.wstring());
   require(!registry.read(), "Native uninstall did not remove verb");
}

}

int wmain(int count, wchar_t* arguments[]) {
   try {
      require(count == 2, "Expected argument probe path");
      Fixture fixture;
      fixture.createdDirectory = std::filesystem::create_directory(fixture.directory);
      require(fixture.createdDirectory, "Test directory already exists");
      HKEY association = nullptr;
      require(RegCreateKeyExW(HKEY_CURRENT_USER, fixture.root.c_str(), 0, nullptr, 0,
                              KEY_READ | KEY_WRITE, nullptr, &association, nullptr) == ERROR_SUCCESS,
              "Create isolated association");
      const wchar_t defaultPlayer[] = L"Existing audio player";
      const auto written = RegSetValueExW(association, nullptr, 0, REG_SZ,
                                         reinterpret_cast<const BYTE*>(defaultPlayer), sizeof(defaultPlayer));
      RegCloseKey(association);
      require(written == ERROR_SUCCESS, "Write default association sentinel");
      const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
      require(SUCCEEDED(initialized), "Initialize shell COM");
      roundTrip(fixture, arguments[1], L"Spaces & \u97f3\u697d !^ ()");
      roundTrip(fixture, arguments[1], L"Literal %1 %L %PATH% %%");
      wchar_t player[128];
      DWORD playerBytes = sizeof(player);
      require(RegGetValueW(HKEY_CURRENT_USER, fixture.root.c_str(), nullptr, RRF_RT_REG_SZ, nullptr,
                            player, &playerBytes) == ERROR_SUCCESS && std::wstring(player) == defaultPlayer,
              "Default association changed");
      CoUninitialize();
      std::cout << "Isolated HKCU lifecycle and ShellExecute UTF-16 argument round trips passed\n";
      return 0;
   } catch (const std::exception& error) {
      std::cerr << error.what() << '\n';
      return 1;
   }
}
