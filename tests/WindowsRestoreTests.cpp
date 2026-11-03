#include "flaccd/mount/WindowsRestore.h"
#include "flaccd/mount/WindowsImgDriveHost.h"

#include <windows.h>

#include <iostream>
#include <stdexcept>

namespace {

void require(bool condition, const char* message) {
   if (!condition) {
      throw std::runtime_error(message);
   }
}

struct TestKey {
   std::wstring path = L"Software\\FlacCD\\RestoreTests\\" + std::to_wstring(GetCurrentProcessId()) +
                       L"-" + std::to_wstring(GetTickCount64());
   bool owned = false;
   ~TestKey() {
      if (owned) {
         RegDeleteTreeW(HKEY_CURRENT_USER, path.c_str());
      }
   }
};

}

int wmain() {
   try {
      TestKey fixture;
      HKEY key = nullptr;
      DWORD disposition = 0;
      require(RegCreateKeyExW(HKEY_CURRENT_USER, fixture.path.c_str(), 0, nullptr, 0, KEY_READ | KEY_WRITE,
                              nullptr, &key, &disposition) == ERROR_SUCCESS, "Create isolated startup registry fixture");
      fixture.owned = disposition == REG_CREATED_NEW_KEY;
      if (!fixture.owned) {
         RegCloseKey(key);
         throw std::runtime_error("Startup test key already exists; preserved");
      }
      const wchar_t sentinel[] = L"unrelated startup program";
      const auto written = RegSetValueExW(key, L"OtherApp", 0, REG_SZ,
                                          reinterpret_cast<const BYTE*>(sentinel), sizeof(sentinel));
      RegCloseKey(key);
      require(written == ERROR_SUCCESS, "Write unrelated startup sentinel");
      flaccd::WindowsRestoreStartup startup(fixture.path);
      const auto command = flaccd::restoreStartupCommand(L"C:\\Program \u97f3 & %\\flaccd.exe", L"C:\\Cache !",
                                                        L"C:\\Img Drive\\ImgDrive.exe");
      flaccd::updateRestoreStartup(startup, L"", command);
      flaccd::updateRestoreStartup(startup, command, command);
      require(startup.read() == command, "Native startup command UTF-16 round trip/idempotence");
      startup.write(L"foreign modified startup");
      bool refused = false;
      try {
         flaccd::updateRestoreStartup(startup, command, {});
      } catch (const std::runtime_error&) {
         refused = true;
      }
      require(refused && startup.read() == L"foreign modified startup", "Foreign startup value not preserved");
      startup.write(command);
      flaccd::updateRestoreStartup(startup, command, {});
      flaccd::updateRestoreStartup(startup, command, {});
      require(!startup.read(), "Native startup removal/idempotence");
      wchar_t value[256];
      DWORD bytes = sizeof(value);
      require(RegGetValueW(HKEY_CURRENT_USER, fixture.path.c_str(), L"OtherApp", RRF_RT_REG_SZ,
                            nullptr, value, &bytes) == ERROR_SUCCESS && std::wstring(value) == sentinel,
              "Unrelated startup value changed");
      flaccd::WindowsImgDriveHost host;
      require(!host.sessionIdentity().empty() && host.sessionIdentity() == host.sessionIdentity(),
              "Interactive logon SID is unavailable or unstable");
      std::cout << "Isolated startup registry lifecycle, Unicode quoting and logon identity passed\n";
      return 0;
   } catch (const std::exception& problem) {
      std::cerr << problem.what() << '\n';
      return 1;
   }
}
