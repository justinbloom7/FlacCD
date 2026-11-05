#include "flaccd/BinCueExporter.h"
#include "flaccd/CddaSectorReader.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace {

void require(bool condition, std::string_view message) {
   if (!condition) {
      throw std::runtime_error(std::string(message));
   }
}

template<typename Operation>
void expectError(Operation operation) {
   try {
      operation();
   } catch (const std::exception&) {
      return;
   }
   throw std::runtime_error("Expected export rejection");
}

bool isSymlinkUnavailable(const std::error_code& error) {
#ifdef _WIN32
   if (error.category() == std::system_category() && error.value() == ERROR_PRIVILEGE_NOT_HELD) {
      return true;
   }
#endif
   // Compare conditions, not message text or platform-specific errno numbers.
   // This also recognizes mapped Windows errors such as missing privileges.
   return error == std::errc::function_not_supported || error == std::errc::not_supported ||
          error == std::errc::operation_not_supported || error == std::errc::permission_denied ||
          error == std::errc::operation_not_permitted;
}

std::error_code createFixtureSymlink(const std::filesystem::path& target,
                                    const std::filesystem::path& link) {
#ifdef _WIN32
   // libc++ maps Win32 errors through errno, losing the original error code.
   // Keep it here so unsupported/permission failures remain distinguishable
   // from invalid paths. Both CreateSymbolicLinkW paths use native separators.
   auto nativeTarget = target;
   auto nativeLink = link;
   nativeTarget.make_preferred();
   nativeLink.make_preferred();
   if (CreateSymbolicLinkW(nativeLink.c_str(), nativeTarget.c_str(), SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE)) {
      return {};
   }
   auto error = GetLastError();
   if (error == ERROR_INVALID_PARAMETER) {
      // Older Windows/filesystems may reject the unprivileged-create flag.
      if (CreateSymbolicLinkW(nativeLink.c_str(), nativeTarget.c_str(), 0)) {
         return {};
      }
      error = GetLastError();
   }
   return {static_cast<int>(error), std::system_category()};
#else
   std::error_code error;
   std::filesystem::create_symlink(target, link, error);
   return error;
#endif
}

void testSymlinkErrorClassification() {
#ifdef _WIN32
   require(isSymlinkUnavailable({ERROR_PRIVILEGE_NOT_HELD, std::system_category()}),
           "Native symlink privilege failure not recognized");
   require(!isSymlinkUnavailable({ERROR_INVALID_PARAMETER, std::system_category()}),
           "Invalid native symlink arguments would be skipped");
#endif
   for (const auto condition : {std::errc::function_not_supported, std::errc::not_supported,
                                std::errc::operation_not_supported, std::errc::permission_denied,
                                std::errc::operation_not_permitted}) {
      require(isSymlinkUnavailable(std::make_error_code(condition)), "Symlink capability error not recognized");
   }
   for (const auto condition : {std::errc::file_exists, std::errc::no_such_file_or_directory,
                                std::errc::io_error, std::errc::no_space_on_device,
                                std::errc::invalid_argument, std::errc::read_only_file_system}) {
      require(!isSymlinkUnavailable(std::make_error_code(condition)), "Unexpected filesystem error would be skipped");
   }
   require(!isSymlinkUnavailable({}), "Successful symlink creation must not be skipped");
}

std::string readFile(const std::filesystem::path& path) {
   std::ifstream input(path, std::ios::binary);
   require(static_cast<bool>(input), "Open test output");
   return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

class TestDirectory {
public:
   explicit TestDirectory(const std::filesystem::path& parent) {
      std::random_device generator;
      for (int attempt = 0; attempt < 100; ++attempt) {
         path = parent / ("bin-cue-test-" + std::to_string(generator()));
         if (std::filesystem::create_directory(path)) {
            return;
         }
      }
      throw std::runtime_error("Cannot create unique test directory");
   }

   ~TestDirectory() {
      std::error_code error;
      std::filesystem::remove_all(path, error);
   }

   std::filesystem::path path;
};

void verifyRoundTrip(const std::filesystem::path& input,
                     const flaccd::TrackLayout& layout,
                     const std::filesystem::path& cuePath,
                     const std::string& expectedPcm) {
   flaccd::exportBinCue(input, layout, cuePath);
   auto binPath = cuePath;
   binPath.replace_extension(".bin");
   require(readFile(binPath) == expectedPcm, "BIN differs from original PCM, including pregap audio");
   const auto cue = flaccd::parseCueTextMetadata(readFile(cuePath));
   require(cue.errors.empty() && cue.fileType == "BINARY", "Reparse generated BINARY CUE");
   require(cue.fileName == binPath.filename().string(), "CUE must reference sibling BIN basename");
   const auto reparsed = flaccd::TrackLayout::fromBinaryCue(std::filesystem::file_size(binPath), cue);
   require(flaccd::compareTrackLayouts(layout, reparsed).empty(), "Round-trip structural layout differs");
   require(layout.getTracks().size() == reparsed.getTracks().size(), "Round-trip track count");
   for (std::size_t position = 0; position < layout.getTracks().size(); ++position) {
      const auto& original = layout.getTracks()[position];
      const auto& result = reparsed.getTracks()[position];
      require(original.title == result.title && original.isrc == result.isrc, "Track title/ISRC lost");
      for (const auto& index : original.indices) {
         for (std::int64_t delta = -1; delta <= 1; ++delta) {
            const auto left = layout.queryLba(index.discLba + delta);
            const auto right = reparsed.queryLba(index.discLba + delta);
            require(left.has_value() == right.has_value(), "Boundary lookup presence differs");
            if (left) {
               require(left->trackNumber == right->trackNumber && left->indexNumber == right->indexNumber &&
                       left->sourceSector == right->sourceSector && left->kind == right->kind,
                       "Boundary lookup changed track/index/source mapping");
            }
         }
      }
   }
   require(!reparsed.queryLba(layout.getLeadOut().discLba), "Lead-out must remain exclusive");
}

void testLayouts(const std::filesystem::path& input,
                 const flaccd::FlacMetadata& metadata,
                 const std::filesystem::path& directory,
                 const std::string& expectedPcm) {
   for (const auto source : {"native", "text"}) {
      const auto layout = flaccd::selectTrackLayout(metadata, source);
      verifyRoundTrip(input, layout, directory / (std::string(source) + ".cue"), expectedPcm);
   }
   const auto fallback = flaccd::parseCueTextMetadata(readFile(directory / "native.cue"));
   require(fallback.title == "Fixture Album" && fallback.performer == "Fixture Album Artist",
           "Vorbis fallback metadata/duplicate tags not preserved");
   const auto descriptions = flaccd::parseCueTextMetadata("TITLE \"Album UTF-8 é\"\nPERFORMER \"Disc Artist\"\nCATALOG 1234567890123\n"
                                                          "FILE fixture.flac FLAC\nTRACK 01 AUDIO\nTITLE \"First\"\nFLAGS PRE DCP\n"
                                                          "ISRC USABC2600001\nINDEX 00 00:00:00\nINDEX 01 00:00:02\nINDEX 02 00:00:07\n"
                                                          "TRACK 02 AUDIO\nTITLE \"Second\"\nPERFORMER \"Guest\"\n"
                                                          "INDEX 00 00:01:00\nINDEX 01 00:01:03\n"
                                                          "TRACK 03 AUDIO\nTITLE \"Third\"\nINDEX 00 00:02:00\nINDEX 01 00:02:00\n");
   const auto described = flaccd::TrackLayout::fromText(metadata, descriptions);
   const auto cuePath = directory / "disc with spaces.cue";
   verifyRoundTrip(input, described, cuePath, expectedPcm);
   const auto cue = flaccd::parseCueTextMetadata(readFile(cuePath));
   require(cue.title == descriptions.title && cue.performer == descriptions.performer,
           "Disc title/performer lost");
   require(cue.tracks[0].performer == "Disc Artist" && cue.tracks[1].performer == "Guest",
           "Inherited/explicit performers lost");
   require(cue.tracks[0].indices[0].sourceSector == 0 && cue.tracks[0].indices[1].sourceSector == 2,
           "Artificial two-second shift introduced");
   require(cue.tracks[1].indices[0].sourceSector == 75 && cue.tracks[2].indices[0].sourceSector == 150,
           "Timestamp seconds/frame conversion failed");
   const auto plainCue = flaccd::parseCueTextMetadata("FILE fixture.flac WAVE\nTRACK 01 AUDIO\nPREGAP 00:00:00\n"
                                                      "INDEX 01 00:00:00\nPOSTGAP 00:00:00\n");
   const auto plain = flaccd::TrackLayout::fromText(metadata, plainCue);
   verifyRoundTrip(input, plain, directory / "plain.cue", expectedPcm);
   require(readFile(directory / "plain.cue").find("INDEX 00") == std::string::npos,
           "Invented INDEX 00 for a disc without a stored pregap");
}

void testErrors(const std::filesystem::path& input,
                const flaccd::FlacMetadata& metadata,
                const std::filesystem::path& directory) {
   const auto layout = flaccd::selectTrackLayout(metadata, "text");
   const auto cuePath = directory / "rejected.cue";
   const auto binPath = directory / "rejected.bin";
   for (const auto directive : {"PREGAP 00:02:00\nINDEX 01 00:00:00\n",
                                "INDEX 01 00:00:00\nPOSTGAP 00:00:01\n"}) {
      const auto gapCue = flaccd::parseCueTextMetadata("FILE fixture.flac WAVE\nTRACK 01 AUDIO\n" + std::string(directive));
      const auto gapLayout = flaccd::TrackLayout::fromText(metadata, gapCue);
      expectError([&] { flaccd::exportBinCue(input, gapLayout, cuePath); });
      require(!std::filesystem::exists(cuePath) && !std::filesystem::exists(binPath),
              "Rejected gap created output");
   }
   for (const auto& existing : {cuePath, binPath}) {
      {
         std::ofstream output(existing);
         output << "Keep existing output";
      }
      expectError([&] { flaccd::exportBinCue(input, layout, cuePath); });
      require(readFile(existing) == "Keep existing output", "Overwrote existing file");
      require(!std::filesystem::exists(existing == cuePath ? binPath : cuePath), "Failed pair left a new output");
      std::filesystem::remove(existing);
   }
   const auto symlinkError = createFixtureSymlink(input, binPath);
   if (symlinkError) {
      if (!isSymlinkUnavailable(symlinkError)) {
         throw std::filesystem::filesystem_error("Cannot create symlink fixture", input, binPath, symlinkError);
      }
      std::cout << "SKIP: BIN/CUE symlink overwrite case: symbolic links unsupported or not permitted: "
                << symlinkError.message() << " (" << symlinkError.category().name() << ':'
                << symlinkError.value() << "). Other overwrite checks still run.\n";
   } else {
      expectError([&] { flaccd::exportBinCue(input, layout, cuePath); });
      require(std::filesystem::is_symlink(binPath), "Overwrote symlink");
      std::filesystem::remove(binPath);
      std::cout << "BIN/CUE symlink overwrite case passed\n";
   }
   std::filesystem::create_hard_link(input, binPath);
   const auto inputSize = std::filesystem::file_size(input);
   expectError([&] { flaccd::exportBinCue(input, layout, cuePath); });
   require(std::filesystem::file_size(input) == inputSize, "Overwrote hard-linked source audio");
   std::filesystem::remove(binPath);

   auto badText = flaccd::parseCueTextMetadata("TITLE bad\nFILE fixture.flac WAVE\nTRACK 01 AUDIO\nINDEX 01 00:00:00\n");
   for (const auto& title : {std::string("quoted \"title\""), std::string("new\nline"), std::string("nul\0value", 9)}) {
      badText.title = title;
      const auto badLayout = flaccd::TrackLayout::fromText(metadata, badText);
      expectError([&] { flaccd::exportBinCue(input, badLayout, cuePath); });
      require(!std::filesystem::exists(binPath) && !std::filesystem::exists(cuePath), "Unsafe CUE text created output");
   }
   const auto truncated = input.parent_path() / "truncated.flac";
   expectError([&] { flaccd::exportBinCue(truncated, layout, cuePath); });
   require(!std::filesystem::exists(binPath) && !std::filesystem::exists(cuePath), "Decode error left partial files");
   expectError([&] { flaccd::TrackLayout::fromBinaryCue(2353, flaccd::parseCueTextMetadata("FILE a.bin BINARY\n")); });
   expectError([&] { flaccd::exportBinCue(input, layout, directory / "wrong.raw"); });
}

}

int main(int argumentCount, char* arguments[]) {
   try {
      require(argumentCount == 2, "Expected fixture directory");
      testSymlinkErrorClassification();
      const auto fixtureDirectory = std::filesystem::absolute(arguments[1]);
      TestDirectory directory(fixtureDirectory);
      const auto input = fixtureDirectory / "fixture-4096.flac";
      const auto metadata = flaccd::FlacMetadataInspector{}.inspect(input);
      testLayouts(input, metadata, directory.path, readFile(fixtureDirectory / "original.raw"));
      testErrors(input, metadata, directory.path);
      std::cout << "BIN/CUE exporter tests passed\n";
   } catch (const std::exception& error) {
      std::cerr << error.what() << '\n';
      return 1;
   }
   return 0;
}
