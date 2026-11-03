#include "flaccd/mount/OpticalDrive.h"

#include <stdexcept>

namespace flaccd {

std::string OpticalDrive::selector() const {
   if (!imgDrive || identity.empty() || letter < 'D' || letter > 'Z') {
      throw std::runtime_error("cannot select an unidentified ImgDrive optical device");
   }
   return std::string(1, letter);
}

std::string describeDrives(const std::vector<OpticalDrive>& drives) {
   std::string text;
   for (const auto& drive : drives) {
      const auto media = drive.media == MediaState::Ready ? "ready" :
                         drive.media == MediaState::Empty ? "empty" : "uncertain";
      text += std::string(1, drive.letter) + ": " + (drive.imgDrive ? "ImgDrive" : "other/unverified") +
              " " + media + " identity=" + drive.identity + " " + drive.description + "\n";
   }
   return text.empty() ? "No visible optical drives (or enumeration unavailable).\n" : text;
}

const OpticalDrive* findDrive(const std::vector<OpticalDrive>& drives, char letter) {
   for (const auto& drive : drives) {
      if (drive.letter == letter) {
         return &drive;
      }
   }
   return nullptr;
}

const OpticalDrive* findDevice(const std::vector<OpticalDrive>& drives, const std::string& identity) {
   const OpticalDrive* found = nullptr;
   for (const auto& drive : drives) {
      if (!identity.empty() && drive.identity == identity) {
         if (found) {
            return nullptr;
         }
         found = &drive;
      }
   }
   return found;
}

}
