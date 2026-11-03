#pragma once

#include <string>
#include <vector>

namespace flaccd {

enum class MediaState { Missing, Empty, Ready, Unknown };

struct OpticalDrive {
   char letter = 0;
   std::string identity;
   std::string description;
   bool imgDrive = false;
   MediaState media = MediaState::Unknown;
   // ImgDrive documents drive letters as identifiers. Never infer its numeric
   // drive ID from a Windows CDROM number, SCSI address, or enumeration order.
   std::string selector() const;
};

std::string describeDrives(const std::vector<OpticalDrive>& drives);
const OpticalDrive* findDrive(const std::vector<OpticalDrive>& drives, char letter);
const OpticalDrive* findDevice(const std::vector<OpticalDrive>& drives, const std::string& identity);

}
