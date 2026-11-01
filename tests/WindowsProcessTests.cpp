#include "flaccd/mount/WindowsImgDriveHost.h"

#include <fstream>
#include <iostream>
#include <random>
#include <stdexcept>

namespace {

void require(bool condition, const char* message) {
   if (!condition) {
      throw std::runtime_error(message);
   }
}

}

int wmain(int argumentCount, wchar_t* arguments[]) {
   std::filesystem::path directory;
   try {
      require(argumentCount == 2, "expected probe executable");
      flaccd::WindowsImgDriveHost host;
      const std::filesystem::path probe(arguments[1]);
      const std::vector<std::string> values{"space name", "音楽 é", "quote\"test", "ends\\", "&;|<>^", ""};
      std::string expected;
      for (const auto& value : values) {
         expected += value + '\n';
      }
      const auto echoed = host.run(probe, values, std::chrono::seconds(5));
      require(!echoed.timedOut && echoed.exitCode == 0 && echoed.diagnostics == expected, "Windows argv round trip");
      const std::vector<std::string> failure{"fail"};
      const auto failed = host.run(probe, failure, std::chrono::seconds(5));
      require(failed.exitCode == 7 && failed.diagnostics == "expected failure", "process diagnostics/exit code");
      const std::vector<std::string> sleep{"sleep"};
      require(host.run(probe, sleep, std::chrono::milliseconds(50)).timedOut, "process timeout");
      const std::vector<std::string> flood{"flood"};
      const auto captured = host.run(probe, flood, std::chrono::seconds(5));
      require(captured.exitCode == 0 && !captured.timedOut && captured.diagnostics.size() == 65536,
              "bounded capture must keep draining a noisy child");
      std::random_device generator;
      directory = std::filesystem::temp_directory_path() / ("flaccd-process-" + std::to_string(generator()));
      require(std::filesystem::create_directory(directory), "create test directory");
      const auto invalid = directory / "invalid.exe";
      {
         std::ofstream output(invalid, std::ios::binary);
         output << "MZinvalid PE file";
      }
      for (const auto& path : {invalid, directory / "missing.exe"}) {
         bool rejected = false;
         try {
            host.run(path, {}, std::chrono::seconds(1));
         } catch (const std::runtime_error&) {
            rejected = true;
         }
         require(rejected, "CreateProcessW must reject missing/invalid executables");
      }
      std::filesystem::remove(invalid);
      std::filesystem::remove(directory);
      std::cout << "Windows process adapter tests passed\n";
   } catch (const std::exception& error) {
      std::cerr << error.what() << '\n';
      // Retain unexpected temporary contents rather than deleting arbitrary files.
      return 1;
   }
   return 0;
}
