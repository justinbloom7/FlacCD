#include "flaccd/shell/WindowsShellRegistry.h"
#include "flaccd/PathText.h"
#include "ProcessArgumentDiagnostics.h"

#include <windows.h>
#include <objbase.h>
#include <shellapi.h>

#include <filesystem>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace {

void require(bool condition, const char* message) {
   if (!condition) {
      throw std::runtime_error(message);
   }
}

struct Fixture {
   // An unassociated ProgID is visible through HKCR without changing .flac,
   // UserChoice, SystemFileAssociations, or any existing application's class.
   std::wstring className = L"FlacCD.ShellTests." + std::to_wstring(GetCurrentProcessId()) +
                            L"." + std::to_wstring(GetTickCount64());
   std::wstring root = L"Software\\Classes\\" + className;
   std::filesystem::path directory = std::filesystem::temp_directory_path() /
      (L"FlacCD shell test " + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
   bool createdDirectory = false;
   bool createdRegistry = false;
   ~Fixture() {
      // This isolated test namespace is never the real file association.
      if (createdRegistry) {
         RegDeleteTreeW(HKEY_CURRENT_USER, root.c_str());
      }
      std::error_code error;
      if (createdDirectory) {
         std::filesystem::remove_all(directory, error);
      }
   }
};

struct ProcessHandle {
   HANDLE value = nullptr;
   ~ProcessHandle() {
      if (value) {
         CloseHandle(value);
      }
   }
};

struct ComApartment {
   ComApartment() {
      const HRESULT result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
      if (FAILED(result)) {
         throw std::runtime_error("CoInitializeEx failed: " + std::to_string(static_cast<std::uint32_t>(result)));
      }
   }
   ~ComApartment() {
      CoUninitialize();
   }
};

std::string errorText(DWORD error) {
   wchar_t buffer[2048]{};
   const DWORD length = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                       nullptr, error, 0, buffer, 2048, nullptr);
   std::ostringstream output;
   output << error << " (0x" << std::hex << std::setw(8) << std::setfill('0') << error << ')';
   if (error == static_cast<DWORD>(CO_E_APPNOTFOUND)) {
      output << " CO_E_APPNOTFOUND";
   }
   if (length > 0) {
      output << ": " << flaccd::pathToUtf8(std::filesystem::path(std::wstring(buffer, length)));
   }
   return output.str();
}

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
   // Verify the command through the same merged classes view that lpClass uses.
   wchar_t registeredCommand[32768];
   DWORD commandBytes = sizeof(registeredCommand);
   const auto commandKey = fixture.className + L"\\shell\\FlacCD.MountAudioCD\\command";
   const auto commandResult = RegGetValueW(HKEY_CLASSES_ROOT, commandKey.c_str(), nullptr, RRF_RT_REG_SZ,
                                          nullptr, registeredCommand, &commandBytes);
   if (commandResult != ERROR_SUCCESS) {
      throw std::runtime_error("Test ProgID command is not visible in HKCR: " + errorText(commandResult));
   }
   require(std::wstring(registeredCommand) == original->at({L"command", L""}), "HKCR test command differs");
   SHELLEXECUTEINFOW request{};
   request.cbSize = sizeof(request);
   request.fMask = SEE_MASK_CLASSNAME | SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI | SEE_MASK_NOASYNC;
   request.lpClass = fixture.className.c_str();
   request.lpVerb = L"FlacCD.MountAudioCD";
   request.lpFile = file.c_str();
   request.nShow = SW_HIDE;
   const BOOL launched = ShellExecuteExW(&request);
   // Capture immediately, and only on failure: success leaves LastError undefined.
   const DWORD launchError = launched ? ERROR_SUCCESS : GetLastError();
   ProcessHandle process{request.hProcess};
   std::ostringstream context;
   context << "\nProgID: " << flaccd::pathToUtf8(std::filesystem::path(fixture.className))
           << "\nRegistry: HKCU\\" << flaccd::pathToUtf8(std::filesystem::path(fixture.root))
           << "\nVerb: FlacCD.MountAudioCD\nFile: " << flaccd::pathToUtf8(file)
           << "\nCommand: " << flaccd::pathToUtf8(std::filesystem::path(registeredCommand))
           << "\nhInstApp: " << reinterpret_cast<std::uintptr_t>(request.hInstApp)
           << "\nhProcess: " << (process.value ? "present" : "null");
   if (!launched) {
      throw std::runtime_error("ShellExecuteExW returned FALSE: " + errorText(launchError) + context.str());
   }
   if (!process.value) {
      throw std::runtime_error("ShellExecuteExW returned TRUE without a process handle; "
                               "this test requires a new probe process (no DDE/delegate reuse). "
                               "GetLastError is not meaningful after success." + context.str());
   }
   const DWORD wait = WaitForSingleObject(process.value, 10000);
   if (wait == WAIT_FAILED) {
      const DWORD waitError = GetLastError();
      throw std::runtime_error("Waiting for shell argument probe failed: " + errorText(waitError) + context.str());
   }
   if (wait != WAIT_OBJECT_0) {
      throw std::runtime_error("Shell argument probe did not exit within 10000 ms; wait result=" +
                               std::to_string(wait) + context.str());
   }
   DWORD exitCode = 0;
   if (!GetExitCodeProcess(process.value, &exitCode)) {
      const DWORD queryError = GetLastError();
      throw std::runtime_error("Querying shell argument probe failed: " + errorText(queryError) + context.str());
   }
   if (exitCode != 0) {
      throw std::runtime_error("Shell argument probe exited with code " + std::to_string(exitCode) + context.str());
   }
   std::ifstream input(std::filesystem::path(executable.wstring() + L".args"), std::ios::binary);
   if (!input) {
      throw std::runtime_error("Shell argument probe output is missing/unreadable" + context.str());
   }
   const std::vector<char> actual((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
   const auto expected = file.wstring();
   const std::string bytes(reinterpret_cast<const char*>(expected.data()), expected.size() * sizeof(wchar_t));
   const std::string actualBytes(actual.begin(), actual.end());
   if (actualBytes != bytes) {
      throw std::runtime_error("Explorer filename UTF-16 round trip differs\nExpected (" +
                               std::to_string(bytes.size()) + " bytes): " + flaccd::test::escapeBytes(bytes) +
                               "\nActual (" + std::to_string(actualBytes.size()) + " bytes): " +
                               flaccd::test::escapeBytes(actualBytes) + context.str());
   }
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
      DWORD disposition = 0;
      const auto created = RegCreateKeyExW(HKEY_CURRENT_USER, fixture.root.c_str(), 0, nullptr, 0,
                                           KEY_READ | KEY_WRITE, nullptr, &association, &disposition);
      if (created != ERROR_SUCCESS) {
         throw std::runtime_error("Create test ProgID: " + errorText(created));
      }
      fixture.createdRegistry = disposition == REG_CREATED_NEW_KEY;
      if (!fixture.createdRegistry) {
         RegCloseKey(association);
         throw std::runtime_error("Test ProgID already exists; preserved without modification");
      }
      const wchar_t defaultPlayer[] = L"Existing audio player";
      const auto written = RegSetValueExW(association, nullptr, 0, REG_SZ,
                                         reinterpret_cast<const BYTE*>(defaultPlayer), sizeof(defaultPlayer));
      RegCloseKey(association);
      require(written == ERROR_SUCCESS, "Write default association sentinel");
      ComApartment apartment;
      roundTrip(fixture, arguments[1], L"Spaces & \u97f3\u697d !^ ()");
      roundTrip(fixture, arguments[1], L"Literal %1 %L %PATH% %%");
      wchar_t player[128];
      DWORD playerBytes = sizeof(player);
      require(RegGetValueW(HKEY_CURRENT_USER, fixture.root.c_str(), nullptr, RRF_RT_REG_SZ, nullptr,
                            player, &playerBytes) == ERROR_SUCCESS && std::wstring(player) == defaultPlayer,
              "Default association changed");
      std::cout << "Isolated HKCU lifecycle and ShellExecute UTF-16 argument round trips passed\n";
      return 0;
   } catch (const std::exception& error) {
      std::cerr << error.what() << '\n';
      return 1;
   }
}
