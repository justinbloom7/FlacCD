#include "flaccd/BinCueExporter.h"

#include <exception>
#include <iostream>
#include <string_view>

namespace {

void printUsage(std::ostream& output) {
   output << "Usage: flaccd-bin-cue <file.flac> <native|text|auto> <output.cue>\n"
          << "Creates output.cue and output.bin; neither file may already exist.\n"
          << "Exports all source audio, including stored INDEX 00 pregaps.\n"
          << "Source-less PREGAP/POSTGAP audio is unsupported.\n";
}

}

int main(int argumentCount, char* arguments[]) {
   if (argumentCount == 2 && std::string_view(arguments[1]) == "--help") {
      printUsage(std::cout);
      return 0;
   }
   if (argumentCount != 4) {
      printUsage(std::cerr);
      return 2;
   }
   try {
      const auto metadata = flaccd::FlacMetadataInspector{}.inspect(arguments[1]);
      const auto layout = flaccd::selectTrackLayout(metadata, arguments[2]);
      flaccd::exportBinCue(arguments[1], layout, arguments[3]);
      std::cout << "Exported " << layout.getLeadOut().sourceSector << " sectors to BIN/CUE\n";
   } catch (const std::exception& error) {
      std::cerr << "flaccd-bin-cue: " << error.what() << '\n';
      return 1;
   }
   return 0;
}
