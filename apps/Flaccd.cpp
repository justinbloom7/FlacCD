#include "flaccd/mount/ImgDriveBackend.h"
#include "flaccd/mount/WindowsImgDriveHost.h"
#include "flaccd/PathText.h"

#include <windows.h>

#include <charconv>
#include <iostream>
#include <set>
#include <stdexcept>

namespace {

void printUsage() {
   std::cout << "Usage:\n"
             << "  flaccd mount FILE.flac --drive D: [--layout native|text|auto] [--no-copy]\n"
             << "  flaccd unmount D:\n"
             << "  flaccd status\n"
             << "  flaccd drives\n"
             << "Options (after command):\n"
             << "  --imgdrive PATH.exe    Explicit installed executable (or FLACCD_IMGDRIVE)\n"
             << "  --cache-dir DIR        Persistent cache; default LocalAppData/FlacCD/ImgDrive/v1\n"
             << "  --no-copy              Mount only without a compressed FLAC copy\n"
             << "  --timeout SECONDS      Per-phase timeout, 1..600; default 30\n"
             << "Auto layout rejects conflicts and prefers agreeing native geometry.\n"
             << "Drive auto-selection is disabled until ImgDrive -l output is qualified.\n"
             << "Use flaccd drives and select an existing, empty, visible ImgDrive drive.\n"
             << "Mounting reuses/creates an adjacent CUE, else stages a hard link or compressed copy.\n"
             << "No BIN is created. Unmount removes only verified FlacCD-owned resources.\n";
}

char parseDrive(std::string_view text) {
   if (text.size() != 2 || text[1] != ':') {
      throw std::invalid_argument("drive must be a letter followed by a colon, such as D:");
   }
   char drive = text[0];
   if (drive >= 'a' && drive <= 'z') {
      drive = static_cast<char>(drive - 'a' + 'A');
   }
   if (drive < 'D' || drive > 'Z') {
      throw std::invalid_argument("drive must be D: through Z:");
   }
   return drive;
}

std::string mediaName(flaccd::MediaState state) {
   switch (state) {
   case flaccd::MediaState::Missing: return "drive missing/hidden";
   case flaccd::MediaState::Empty: return "no media (stale if recorded mounted)";
   case flaccd::MediaState::Ready: return "media ready (current image identity not queried)";
   case flaccd::MediaState::Unknown: return "media status unknown";
   }
   return "unknown";
}

int run(const std::vector<std::string>& arguments) {
   if (arguments.size() == 1 && arguments[0] == "--help") {
      printUsage();
      return 0;
   }
   if (arguments.empty()) {
      printUsage();
      return 2;
   }
   const auto& command = arguments[0];
   if (command != "mount" && command != "unmount" && command != "status" && command != "drives") {
      throw std::invalid_argument("unknown command; use flaccd --help");
   }
   std::optional<char> drive;
   std::filesystem::path input;
   std::size_t position = 1;
   if (command == "mount" || command == "unmount") {
      if (arguments.size() <= position || arguments[position].starts_with("--")) {
         throw std::invalid_argument("missing FLAC filename or drive");
      }
      if (command == "mount") {
         input = flaccd::pathFromUtf8(arguments[position++]);
      } else {
         drive = parseDrive(arguments[position++]);
      }
   }
   std::filesystem::path executable;
   std::filesystem::path cache;
   std::string layout = "auto";
   bool noCopy = false;
   std::chrono::seconds timeout(30);
   std::set<std::string> seen;
   while (position < arguments.size()) {
      const auto option = arguments[position++];
      if (!seen.insert(option).second) {
         throw std::invalid_argument("duplicate option: " + option);
      }
      if (option == "--no-copy" && command == "mount") {
         noCopy = true;
         continue;
      }
      if (position == arguments.size()) {
         throw std::invalid_argument("duplicate option or missing value: " + option);
      }
      const auto& value = arguments[position++];
      if (option == "--imgdrive") {
         executable = flaccd::pathFromUtf8(value);
      } else if (option == "--cache-dir") {
         cache = flaccd::pathFromUtf8(value);
      } else if (option == "--timeout") {
         unsigned seconds = 0;
         const auto result = std::from_chars(value.data(), value.data() + value.size(), seconds);
         if (result.ec != std::errc{} || result.ptr != value.data() + value.size() || seconds == 0 || seconds > 600) {
            throw std::invalid_argument("--timeout must be 1..600 seconds");
         }
         timeout = std::chrono::seconds(seconds);
      } else if (option == "--layout" && command == "mount") {
         if (value != "auto" && value != "native" && value != "text") {
            throw std::invalid_argument("--layout must be auto, native, or text");
         }
         layout = value;
      } else if (option == "--drive" && command == "mount") {
         drive = parseDrive(value);
      } else {
         throw std::invalid_argument("unknown or inapplicable option: " + option);
      }
   }
   executable = flaccd::findImgDrive(executable);
   if (cache.empty()) {
      cache = flaccd::defaultMountCache();
   }
   std::cerr << "ImgDrive: " << flaccd::pathToUtf8(executable) << "\nCache: " << flaccd::pathToUtf8(cache) << '\n';
   flaccd::WindowsImgDriveHost host;
   flaccd::ImgDriveBackend backend(host, executable, cache, timeout, nullptr, [](const std::string& message) {
      std::cerr << message << '\n';
   });
   if (command == "mount") {
      const auto record = backend.mount(input, drive, layout, [](const flaccd::MountRecord& pending) {
         const auto cue = flaccd::parseCueTextMetadata(pending.cueText);
         std::cerr << "Original FLAC: " << flaccd::pathToUtf8(pending.sourcePath)
                   << "\nPersistent CUE: " << flaccd::pathToUtf8(pending.cuePath)
                   << "\nFILE reference: \"" << cue.fileName << "\" " << cue.fileType
                   << "\nStrategy: " << flaccd::mountStrategyName(pending)
                   << "\nAudio resource: " << flaccd::pathToUtf8(pending.audioPath)
                   << "\nMatching CUE/audio resources verified before invocation.\n";
      }, noCopy);
      std::cout << "Mounted " << flaccd::pathToUtf8(record.sourcePath) << " on " << record.drive << ":\n"
                << "Persistent CUE: " << flaccd::pathToUtf8(record.cuePath) << '\n';
   } else if (command == "unmount") {
      backend.unmount(*drive);
      std::cout << "Unmount confirmed; owned resources/state removed; reused user CUE retained.\n";
   } else if (command == "drives") {
      const auto listing = backend.drives();
      std::cout << "ImgDrive -l output (not parsed):\n" << listing;
      if (listing.empty()) {
         std::cout << "No captured listing. Select/check an empty virtual drive in ImgDrive's GUI.\n";
      }
   } else {
      const auto mounts = backend.status();
      if (mounts.empty()) {
         std::cout << "No FlacCD mount records. Use flaccd drives for ImgDrive's complete list.\n";
      }
      for (const auto& item : mounts) {
         std::cout << item.record.drive << ": recorded " << flaccd::mountStateName(item.record.state)
                   << "; " << mediaName(item.media) << "\n  FLAC: " << flaccd::pathToUtf8(item.record.sourcePath)
                   << "\n  Strategy: " << flaccd::mountStrategyName(item.record)
                   << "\n  Audio: " << flaccd::pathToUtf8(item.record.audioPath)
                   << "\n  CUE: " << flaccd::pathToUtf8(item.record.cuePath) << '\n';
      }
   }
   return 0;
}

}

int wmain(int argumentCount, wchar_t* arguments[]) {
   SetConsoleOutputCP(CP_UTF8);
   try {
      std::vector<std::string> text;
      for (int position = 1; position < argumentCount; ++position) {
         text.push_back(flaccd::pathToUtf8(std::filesystem::path(arguments[position])));
      }
      return run(text);
   } catch (const std::invalid_argument& error) {
      std::cerr << "flaccd: " << error.what() << '\n';
      return 2;
   } catch (const std::exception& error) {
      std::cerr << "flaccd: " << error.what() << '\n';
      return 1;
   }
}
