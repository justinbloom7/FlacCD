#pragma once

#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace flaccd {

// Keys are relative to the dedicated verb; values are REG_SZ only.
using ShellValues = std::map<std::pair<std::wstring, std::wstring>, std::wstring>;

class ShellRegistry {
public:
   virtual ~ShellRegistry() = default;
   virtual std::optional<ShellValues> read() = 0;
   virtual void create(const ShellValues& values) = 0;
   virtual void remove(const ShellValues& expected) = 0;
};

std::wstring shellMountCommand(std::wstring_view executable);
ShellValues shellRegistration(std::wstring_view executable);
std::string runShellCommand(ShellRegistry& registry, std::string_view action, std::wstring_view executable);

}
