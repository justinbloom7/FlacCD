#include "flaccd/AudioExporter.h"
#include "flaccd/BinCueExporter.h"

#include <charconv>
#include <exception>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

enum class Mode { None, All, Track, Tracks, Split, CueOnly, Legacy };

struct Options {
   std::filesystem::path input;
   std::string layout = "auto";
   std::string format = "raw";
   std::filesystem::path output;
   std::filesystem::path outputDirectory;
   std::string binName;
   Mode mode = Mode::None;
   std::uint32_t firstTrack = 0;
   std::uint32_t lastTrack = 0;
   bool indexOneOnly = false;
   bool overwrite = false;
   std::int64_t startLba = 0;
   std::uint64_t sectorCount = 0;
};

class UsageError : public std::invalid_argument {
public:
   using std::invalid_argument::invalid_argument;
};

template<typename Integer>
Integer parseInteger(std::string_view text) {
   if (text.empty()) {
      throw UsageError("missing integer");
   }
   Integer value = 0;
   const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
   if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
      throw UsageError("invalid integer: " + std::string(text));
   }
   return value;
}

Options parseOptions(int argumentCount, char* arguments[]) {
   if (argumentCount < 3) {
      throw UsageError("missing input or export mode");
   }
   Options options;
   options.input = arguments[1];
   if (!std::string_view(arguments[2]).starts_with("--")) {
      if (argumentCount != 6) {
         throw UsageError("legacy usage requires layout, start LBA, sector count, and output");
      }
      options.mode = Mode::Legacy;
      options.layout = arguments[2];
      options.startLba = parseInteger<std::int64_t>(arguments[3]);
      options.sectorCount = parseInteger<std::uint64_t>(arguments[4]);
      options.output = arguments[5];
      return options;
   }
   std::set<std::string_view> seen;
   const auto setMode = [&](Mode mode) {
      if (options.mode != Mode::None) {
         throw UsageError("choose exactly one export mode");
      }
      options.mode = mode;
   };
   for (int position = 2; position < argumentCount; ++position) {
      const std::string_view option(arguments[position]);
      if (!seen.insert(option).second) {
         throw UsageError("duplicate option: " + std::string(option));
      }
      const auto value = [&]() -> std::string_view {
         if (position + 1 >= argumentCount || std::string_view(arguments[position + 1]).starts_with("--")) {
            throw UsageError("missing value for " + std::string(option));
         }
         return arguments[++position];
      };
      if (option == "--all") {
         setMode(Mode::All);
      } else if (option == "--track") {
         setMode(Mode::Track);
         options.firstTrack = options.lastTrack = parseInteger<std::uint32_t>(value());
      } else if (option == "--tracks") {
         setMode(Mode::Tracks);
         const auto range = value();
         const auto separator = range.find('-');
         if (separator == std::string_view::npos) {
            throw UsageError("--tracks requires a range such as 2-5");
         }
         options.firstTrack = parseInteger<std::uint32_t>(range.substr(0, separator));
         options.lastTrack = parseInteger<std::uint32_t>(range.substr(separator + 1));
      } else if (option == "--split-tracks") {
         setMode(Mode::Split);
      } else if (option == "--cue-only") {
         setMode(Mode::CueOnly);
      } else if (option == "--layout") {
         options.layout = value();
      } else if (option == "--format") {
         options.format = value();
      } else if (option == "--output") {
         options.output = value();
      } else if (option == "--output-dir") {
         options.outputDirectory = value();
      } else if (option == "--bin-file") {
         options.binName = value();
      } else if (option == "--index01-only") {
         options.indexOneOnly = true;
      } else if (option == "--overwrite") {
         options.overwrite = true;
      } else {
         throw UsageError("unknown option: " + std::string(option));
      }
   }
   if (options.mode == Mode::None) {
      throw UsageError("select --all, --track, --tracks, --split-tracks, or --cue-only");
   }
   if (options.layout != "auto" && options.layout != "native" && options.layout != "text") {
      throw UsageError("--layout must be native, text, or auto");
   }
   if (options.format != "raw" && options.format != "bin-cue") {
      throw UsageError("--format must be raw or bin-cue; WAV is not supported");
   }
   if (options.mode == Mode::Split) {
      if (options.outputDirectory.empty() || !options.output.empty()) {
         throw UsageError("--split-tracks requires --output-dir and forbids --output");
      }
   } else if (options.output.empty() || !options.outputDirectory.empty()) {
      throw UsageError("this mode requires --output and forbids --output-dir");
   }
   if (options.format == "bin-cue" && options.mode != Mode::All) {
      throw UsageError("--format bin-cue requires --all");
   }
   if (options.indexOneOnly && options.mode != Mode::Track && options.mode != Mode::Tracks && options.mode != Mode::Split) {
      throw UsageError("--index01-only requires a track export mode");
   }
   if (options.mode == Mode::CueOnly) {
      if (options.binName.empty() || seen.contains("--format")) {
         throw UsageError("--cue-only requires --bin-file and forbids --format");
      }
   } else if (!options.binName.empty()) {
      throw UsageError("--bin-file requires --cue-only");
   }
   return options;
}

void runExport(const Options& options) {
   const auto metadata = flaccd::FlacMetadataInspector{}.inspect(options.input);
   const auto layout = flaccd::selectTrackLayout(metadata, options.layout);
   std::cerr << "Layout: " << options.layout << "; " << layout.getTracks().size() << " audio tracks\n";
   std::uint64_t lastReported = 0;
   const auto progress = [&](std::uint64_t completed, std::uint64_t total) {
      if (completed == total || completed - lastReported >= 75 * 10) {
         std::cerr << "Decoded " << completed << '/' << total << " sectors (staged)\n";
         lastReported = completed;
      }
   };
   if (options.mode == Mode::CueOnly) {
      if (flaccd::getDiscRange(layout).beginLba != 0) {
         throw std::invalid_argument("CUE-only assumes a BIN starting at LBA 0, but audio precedes LBA 0; "
                                     "use --all --format bin-cue to preserve the first-track pregap");
      }
      flaccd::exportCueSheet(options.input, layout, options.output, options.binName, options.overwrite);
      std::cout << "Wrote CUE; referenced BIN must contain the complete disc starting at LBA 0.\n";
      return;
   }
   if (options.format == "bin-cue") {
      auto cuePath = options.output;
      cuePath += ".cue";
      flaccd::exportBinCue(options.input, layout, cuePath, options.overwrite, progress);
      std::cout << "Published complete BIN/CUE pair: " << cuePath << '\n';
      return;
   }
   std::vector<flaccd::RawExport> outputs;
   if (options.mode == Mode::All) {
      outputs.push_back({options.output, {flaccd::getDiscRange(layout)}});
   } else if (options.mode == Mode::Legacy) {
      const auto disc = flaccd::getDiscRange(layout);
      if (options.startLba < disc.beginLba || options.startLba >= disc.endLba || options.sectorCount == 0) {
         throw std::out_of_range("requested range is empty or starts outside the disc");
      }
      const auto available = static_cast<std::uint64_t>(disc.endLba) - static_cast<std::uint64_t>(options.startLba);
      if (options.sectorCount > available ||
          options.sectorCount > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
         throw std::out_of_range("requested sector count extends beyond lead-out");
      }
      const auto end = options.startLba + static_cast<std::int64_t>(options.sectorCount);
      outputs.push_back({options.output, {{options.startLba, end}}});
   } else if (options.mode == Mode::Split) {
      for (const auto& track : layout.getTracks()) {
         std::ostringstream name;
         name << "Track" << std::setfill('0') << std::setw(2) << track.number << ".raw";
         outputs.push_back({options.outputDirectory / name.str(),
                            flaccd::getTrackRanges(layout, track.number, track.number, options.indexOneOnly)});
      }
      // Validate all ranges before creating even the destination directory.
      for (const auto& output : outputs) {
         flaccd::validateSectorRanges(layout, output.ranges);
      }
      std::filesystem::create_directories(options.outputDirectory);
   } else {
      outputs.push_back({options.output,
                         flaccd::getTrackRanges(layout, options.firstTrack, options.lastTrack, options.indexOneOnly)});
   }
   flaccd::exportRaw(options.input, layout, outputs, options.overwrite, progress);
   std::cout << "Published " << outputs.size() << " complete raw CDDA file(s).\n";
}

void printUsage(std::ostream& output) {
   output << "Usage: flaccd-export <file.flac> <native|text> <start-lba> <sector-count> <output.raw>\n"
          << "   or: flaccd-export <file.flac> <mode> [options]\n"
          << "Modes (choose one):\n"
          << "  --all                         Export complete source, including stored first-track pregap\n"
          << "  --track N                     Export actual CD track N\n"
          << "  --tracks FIRST-LAST           Concatenate selected tracks\n"
          << "  --split-tracks                Export Track01.raw, Track02.raw, ...\n"
          << "  --cue-only                    Write external CUE for a complete BIN starting at LBA 0\n"
          << "Options:\n"
          << "  --layout native|text|auto     Default auto; conflicts are errors\n"
          << "  --format raw|bin-cue          Default raw; bin-cue requires --all\n"
          << "  --output PATH                 Raw/CUE filename, or BIN/CUE basename (without extension)\n"
          << "  --output-dir DIR              Required for --split-tracks; created if needed\n"
          << "  --bin-file NAME.bin           Required for --cue-only; sibling filename, not a path\n"
          << "  --index01-only                Omit each selected track's own stored INDEX 00 pregap\n"
          << "  --overwrite                   Replace existing regular outputs; never input audio\n"
          << "Raw/BIN: signed 16-bit little-endian stereo PCM, 2352 bytes/sector.\n"
          << "Default track slices include their own INDEX 00 and stop before the next track's pregap.\n"
          << "Nonzero source-less PREGAP/POSTGAP regions in selected audio are rejected.\n"
          << "Outputs are staged; only complete files are published. WAV is not supported.\n";
}

}

int main(int argumentCount, char* arguments[]) {
   if (argumentCount == 2 && std::string_view(arguments[1]) == "--help") {
      printUsage(std::cout);
      return 0;
   }
   try {
      runExport(parseOptions(argumentCount, arguments));
   } catch (const UsageError& error) {
      std::cerr << "flaccd-export: " << error.what() << '\n';
      printUsage(std::cerr);
      return 2;
   } catch (const std::exception& error) {
      std::cerr << "flaccd-export: " << error.what() << '\n';
      return 1;
   }
   return 0;
}
