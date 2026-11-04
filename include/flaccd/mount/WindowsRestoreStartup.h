#pragma once

#include "flaccd/mount/RestoreStartup.h"

namespace flaccd {

class WindowsRestoreStartup final : public RestoreStartup {
public:
   explicit WindowsRestoreStartup(std::wstring registryRoot = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run");
   std::optional<std::wstring> read() override;
   void write(const std::wstring& command) override;
   void remove() override;
private:
   std::wstring root;
};

}
