#pragma once

#include "flaccd/shell/ShellRegistration.h"

#include <memory>

namespace flaccd {

// The optional root supports isolated HKCU tests. All mutations are confined
// to the fixed FlacCD.MountAudioCD child, never the association/default value.
class WindowsShellRegistry final : public ShellRegistry {
public:
   explicit WindowsShellRegistry(std::wstring root = L"Software\\Classes\\SystemFileAssociations\\.flac\\shell");
   ~WindowsShellRegistry() override;
   std::optional<ShellValues> read() override;
   void create(const ShellValues& values) override;
   void remove(const ShellValues& expected) override;

private:
   struct State;
   std::unique_ptr<State> state;
};

std::wstring shellExecutablePath();

}
