#include "flaccd/shell/ShellRegistration.h"

#include <functional>
#include <iostream>
#include <stdexcept>

namespace {

void require(bool condition, const char* message) {
   if (!condition) {
      throw std::runtime_error(message);
   }
}

void fails(const std::function<void()>& operation) {
   try {
      operation();
   } catch (const std::exception&) {
      return;
   }
   throw std::runtime_error("Expected registration failure");
}

class Registry final : public flaccd::ShellRegistry {
public:
   std::optional<flaccd::ShellValues> values;
   bool denyWrites = false;
   int writes = 0;
   std::optional<flaccd::ShellValues> read() override {
      return values;
   }
   void create(const flaccd::ShellValues& requested) override {
      require(!denyWrites, "Registry access denied");
      require(!values, "Exclusive creation conflict");
      values = requested;
      ++writes;
   }
   void remove(const flaccd::ShellValues& expected) override {
      require(!denyWrites && values == expected, "Removal access/conflict error");
      values.reset();
      ++writes;
   }
};

}

int main() {
   try {
      const std::wstring executable = L"C:\\Audio tools & (test)\\\u97f3\u697d\\flaccd.exe";
      require(flaccd::shellMountCommand(executable) == L"\"" + executable + L"\" mount \"%1\"",
              "Executable or filename placeholder not quoted exactly");
      require(flaccd::shellMountCommand(L"C:\\%1 %L %PATH% !^;'\\flaccd.exe") ==
                 L"\"C:\\%%1 %%L %%PATH%% !^;'\\flaccd.exe\" mount \"%1\"",
              "Executable template percent escaping");
      require(flaccd::shellMountCommand(L"\\\\server\\share name\\flaccd.exe").starts_with(L"\"\\\\server"),
              "UNC executable path");
      require(flaccd::shellMountCommand(L"C:\\FlacCD.ExE") == L"\"C:\\FlacCD.ExE\" mount \"%1\"",
              "Executable case not preserved");
      for (const auto path : {L"flaccd.exe", L"C:flaccd.exe", L"C:\\bad\"\\flaccd.exe", L"C:\\bad\n.exe"}) {
         fails([&] { flaccd::shellMountCommand(path); });
      }
      Registry registry;
      require(flaccd::runShellCommand(registry, "status", executable).find("not installed") != std::string::npos,
              "Missing registration status");
      flaccd::runShellCommand(registry, "uninstall", executable);
      require(registry.writes == 0, "Missing uninstall modified registry");
      flaccd::runShellCommand(registry, "install", executable);
      const auto installed = registry.values;
      flaccd::runShellCommand(registry, "install", executable);
      flaccd::runShellCommand(registry, "status", executable);
      require(registry.writes == 1 && registry.values == installed, "Install/status not idempotent");
      fails([&] { flaccd::runShellCommand(registry, "install", L"D:\\flaccd.exe"); });
      require(registry.values == installed, "Another installation overwritten");
      (*registry.values)[{L"command", L""}] = L"user edited command";
      const auto modified = registry.values;
      for (const auto action : {"install", "uninstall", "status"}) {
         fails([&] { flaccd::runShellCommand(registry, action, executable); });
         require(registry.values == modified, "Modified registration not preserved");
      }
      registry.values = installed;
      (*registry.values)[{L"", L"Other"}] = L"foreign";
      fails([&] { flaccd::runShellCommand(registry, "uninstall", executable); });
      registry.values = installed;
      registry.denyWrites = true;
      fails([&] { flaccd::runShellCommand(registry, "uninstall", executable); });
      require(registry.values == installed, "Failed removal lost registration");
      registry.denyWrites = false;
      flaccd::runShellCommand(registry, "uninstall", L"D:\\flaccd.exe");
      require(!registry.values, "Owned registration not removed from another executable location");
      registry.denyWrites = true;
      fails([&] { flaccd::runShellCommand(registry, "install", executable); });
      require(!registry.values, "Failed install created registration");
      fails([&] { flaccd::runShellCommand(registry, "bad-action", executable); });
      std::cout << "Shell registration policy and command construction passed\n";
      return 0;
   } catch (const std::exception& error) {
      std::cerr << error.what() << '\n';
      return 1;
   }
}
