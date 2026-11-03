#include "flaccd/mount/WindowsImgDriveHost.h"
#include "flaccd/PathText.h"
#include "flaccd/mount/SessionIdentity.h"

#include <windows.h>
#include <winioctl.h>
#include <ntddcdrm.h>
#include <shlobj.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <sddl.h>
#include <ntsecapi.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <limits>
#include <stdexcept>
#include <thread>

namespace flaccd {
namespace {

class WindowsHandle {
public:
   explicit WindowsHandle(HANDLE value = nullptr) : handle(value) {
   }
   ~WindowsHandle() {
      if (handle && handle != INVALID_HANDLE_VALUE) {
         CloseHandle(handle);
      }
   }
   WindowsHandle(const WindowsHandle&) = delete;
   WindowsHandle& operator=(const WindowsHandle&) = delete;
   HANDLE get() const {
      return handle;
   }
private:
   HANDLE handle;
};

std::string windowsError(std::string_view operation) {
   const auto error = GetLastError();
   std::array<wchar_t, 2048> message{};
   FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, error, 0,
                  message.data(), static_cast<DWORD>(message.size()), nullptr);
   return std::string(operation) + " (Windows error " + std::to_string(error) + "): " +
          pathToUtf8(std::filesystem::path(message.data()));
}

std::wstring environment(const wchar_t* name) {
   const auto length = GetEnvironmentVariableW(name, nullptr, 0);
   if (length == 0) {
      return {};
   }
   std::wstring result(length, L'\0');
   const auto actual = GetEnvironmentVariableW(name, result.data(), length);
   if (actual == 0 || actual >= length) {
      throw std::runtime_error("environment variable changed while reading");
   }
   result.resize(actual);
   return result;
}

std::filesystem::path registeredImgDrive(HKEY hive, REGSAM view) {
   constexpr const wchar_t* KEY = L"Software\\Microsoft\\Windows\\CurrentVersion\\App Paths\\ImgDrive.exe";
   HKEY key = nullptr;
   if (RegOpenKeyExW(hive, KEY, 0, KEY_READ | view, &key) != ERROR_SUCCESS) {
      return {};
   }
   std::array<wchar_t, 32768> value{};
   DWORD size = static_cast<DWORD>(value.size() * sizeof(wchar_t));
   DWORD type = 0;
   const auto status = RegQueryValueExW(key, nullptr, nullptr, &type,
                                       reinterpret_cast<BYTE*>(value.data()), &size);
   RegCloseKey(key);
   if (status != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ) ||
       size < sizeof(wchar_t) || size > value.size() * sizeof(wchar_t) ||
       size % sizeof(wchar_t) != 0 || value[size / sizeof(wchar_t) - 1] != L'\0') {
      return {};
   }
   std::wstring text(value.data());
   if (type == REG_EXPAND_SZ) {
      std::array<wchar_t, 32768> expanded{};
      const auto count = ExpandEnvironmentStringsW(text.c_str(), expanded.data(), static_cast<DWORD>(expanded.size()));
      if (count == 0 || count > expanded.size()) {
         return {};
      }
      text = expanded.data();
   }
   if (text.size() >= 2 && text.front() == L'"' && text.back() == L'"') {
      text = text.substr(1, text.size() - 2);
   }
   return std::filesystem::path(text);
}

std::wstring rootPath(char drive) {
   std::wstring root = L"D:\\";
   root[0] = static_cast<wchar_t>(drive);
   return root;
}

HANDLE openDrive(char drive, DWORD access = GENERIC_READ) {
   std::wstring device = L"\\\\.\\D:";
   device[4] = static_cast<wchar_t>(drive);
   return CreateFileW(device.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE,
                       nullptr, OPEN_EXISTING, 0, nullptr);
}

std::int64_t tocLba(const TRACK_DATA& track) {
   if (track.Address[2] >= 60 || track.Address[3] >= 75) {
      throw std::runtime_error("invalid optical TOC timestamp");
   }
   return (static_cast<std::int64_t>(track.Address[1]) * 60 + track.Address[2]) * 75 + track.Address[3] - 150;
}

class WindowsMountLock : public MountLock {
public:
   WindowsMountLock(const std::filesystem::path& path, std::chrono::milliseconds timeout) {
      // Stable case-folded FNV-1a identifier, not implementation-specific std::hash.
      auto name = std::filesystem::weakly_canonical(path).wstring();
      CharUpperBuffW(name.data(), static_cast<DWORD>(name.size()));
      std::uint64_t hash = 14695981039346656037ULL;
      for (const auto character : name) {
         hash = (hash ^ static_cast<std::uint16_t>(character)) * 1099511628211ULL;
      }
      const auto mutexName = L"Global\\FlacCD-ImgDrive-" + std::to_wstring(hash);
      handle = CreateMutexW(nullptr, FALSE, mutexName.c_str());
      if (!handle) {
         throw std::runtime_error(windowsError("CreateMutexW"));
      }
      const auto wait = WaitForSingleObject(handle, static_cast<DWORD>(timeout.count()));
      if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED) {
         CloseHandle(handle);
         handle = nullptr;
         throw std::runtime_error("another FlacCD command holds the mount-state lock");
      }
   }
   ~WindowsMountLock() override {
      ReleaseMutex(handle);
      CloseHandle(handle);
   }
private:
   HANDLE handle = nullptr;
};

}

std::filesystem::path findImgDrive(const std::filesystem::path& explicitPath) {
   if (!explicitPath.empty()) {
      return explicitPath;
   }
   const auto configured = environment(L"FLACCD_IMGDRIVE");
   if (!configured.empty()) {
      return std::filesystem::path(configured);
   }
   for (const auto hive : {HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE}) {
      for (const auto view : {KEY_WOW64_64KEY, KEY_WOW64_32KEY}) {
         const auto candidate = registeredImgDrive(hive, view);
         if (candidate.is_absolute() && std::filesystem::is_regular_file(candidate)) {
            return candidate;
         }
      }
   }
   // Search only explicit PATH directories: exclude current-directory executable
   // lookup, which could run an unrelated ImgDrive.exe from a music directory.
   const auto paths = environment(L"PATH");
   std::size_t begin = 0;
   while (begin <= paths.size()) {
      const auto end = paths.find(L';', begin);
      auto directory = paths.substr(begin, end == std::wstring::npos ? end : end - begin);
      if (directory.size() >= 2 && directory.front() == L'"' && directory.back() == L'"') {
         directory = directory.substr(1, directory.size() - 2);
      }
      if (!directory.empty() && std::filesystem::path(directory).is_absolute()) {
         const auto candidate = std::filesystem::path(directory) / L"ImgDrive.exe";
         if (std::filesystem::is_regular_file(candidate)) {
            return candidate;
         }
      }
      if (end == std::wstring::npos) {
         break;
      }
      begin = end + 1;
   }
   throw std::runtime_error("ImgDrive not detected; install it separately or pass --imgdrive C:\\Path\\ImgDrive.exe");
}

std::filesystem::path defaultMountCache() {
   PWSTR path = nullptr;
   if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &path))) {
      throw std::runtime_error("cannot locate LocalAppData for persistent mount cache");
   }
   const auto result = std::filesystem::path(path) / L"FlacCD" / L"ImgDrive" / L"v1";
   CoTaskMemFree(path);
   return result;
}

ProcessResult WindowsImgDriveHost::run(const std::filesystem::path& executable,
                                       std::span<const std::string> arguments,
                                       std::chrono::milliseconds timeout) {
   std::wstring command = quoteWindowsArgument(executable.wstring());
   for (const auto& argument : arguments) {
      command += L' ';
      command += quoteWindowsArgument(pathFromUtf8(argument).wstring());
   }
   if (command.size() >= 32767) {
      throw std::invalid_argument("ImgDrive command exceeds Windows command-line length");
   }
   SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
   HANDLE rawRead = nullptr;
   HANDLE rawWrite = nullptr;
   if (!CreatePipe(&rawRead, &rawWrite, &security, 0)) {
      throw std::runtime_error(windowsError("CreatePipe"));
   }
   WindowsHandle readPipe(rawRead);
   WindowsHandle writePipe(rawWrite);
   if (!SetHandleInformation(readPipe.get(), HANDLE_FLAG_INHERIT, 0)) {
      throw std::runtime_error(windowsError("SetHandleInformation"));
   }
   WindowsHandle nullInput(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                       &security, OPEN_EXISTING, 0, nullptr));
   if (nullInput.get() == INVALID_HANDLE_VALUE) {
      throw std::runtime_error(windowsError("open NUL"));
   }
   STARTUPINFOW startup{};
   startup.cb = sizeof(startup);
   startup.dwFlags = STARTF_USESTDHANDLES;
   startup.hStdOutput = writePipe.get();
   startup.hStdError = writePipe.get();
   startup.hStdInput = nullInput.get();
   PROCESS_INFORMATION process{};
   if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                        nullptr, nullptr, &startup, &process)) {
      throw std::runtime_error(windowsError("CreateProcessW for ImgDrive"));
   }
   WindowsHandle processHandle(process.hProcess);
   WindowsHandle threadHandle(process.hThread);
   ProcessResult result;
   const auto deadline = std::chrono::steady_clock::now() + timeout;
   const auto drain = [&] {
      // Keep draining after reaching the capture cap so the child cannot block.
      // Bound each pass so a noisy process cannot evade the timeout check.
      for (int iteration = 0; iteration < 16; ++iteration) {
         DWORD available = 0;
         if (!PeekNamedPipe(readPipe.get(), nullptr, 0, nullptr, &available, nullptr) || available == 0) {
            break;
         }
         std::array<char, 4096> bytes{};
         DWORD count = 0;
         if (!ReadFile(readPipe.get(), bytes.data(), std::min<DWORD>(available, static_cast<DWORD>(bytes.size())), &count, nullptr)) {
            break;
         }
         constexpr std::size_t MAX_DIAGNOSTICS = 65536;
         result.diagnostics.append(bytes.data(), std::min<std::size_t>(count, MAX_DIAGNOSTICS - result.diagnostics.size()));
      }
   };
   for (;;) {
      drain();
      const auto wait = WaitForSingleObject(processHandle.get(), 10);
      if (wait == WAIT_OBJECT_0) {
         break;
      }
      if (wait == WAIT_FAILED) {
         throw std::runtime_error(windowsError("waiting for ImgDrive"));
      }
      if (std::chrono::steady_clock::now() >= deadline) {
         result.timedOut = true;
         // Only terminate the process created here, never a previously running
         // ImgDrive GUI. A driver/GUI operation may still have completed.
         TerminateProcess(processHandle.get(), ERROR_TIMEOUT);
         WaitForSingleObject(processHandle.get(), 1000);
         break;
      }
   }
   drain();
   DWORD exitCode = 0;
   if (!GetExitCodeProcess(processHandle.get(), &exitCode)) {
      throw std::runtime_error(windowsError("GetExitCodeProcess"));
   }
   result.exitCode = exitCode;
   return result;
}

MediaState WindowsImgDriveHost::inspectDrive(char drive) {
   const auto type = GetDriveTypeW(rootPath(drive).c_str());
   if (type == DRIVE_NO_ROOT_DIR || type == DRIVE_UNKNOWN) {
      return MediaState::Missing;
   }
   if (type != DRIVE_CDROM) {
      return MediaState::Unknown;
   }
   WindowsHandle device(openDrive(drive, FILE_READ_ATTRIBUTES));
   if (device.get() == INVALID_HANDLE_VALUE) {
      return MediaState::Unknown;
   }
   DWORD returned = 0;
   if (DeviceIoControl(device.get(), IOCTL_STORAGE_CHECK_VERIFY2, nullptr, 0, nullptr, 0, &returned, nullptr)) {
      return MediaState::Ready;
   }
   const DWORD error = GetLastError();
   return error == ERROR_NOT_READY || error == ERROR_NO_MEDIA_IN_DRIVE ? MediaState::Empty : MediaState::Unknown;
}

bool WindowsImgDriveHost::matchesLayout(char drive, const TrackLayout& layout) {
   const auto reject = [&](const std::string& reason) {
      tocDiagnostic = std::string(1, drive) + ": " + reason;
      return false;
   };
   WindowsHandle device(openDrive(drive));
   if (device.get() == INVALID_HANDLE_VALUE) {
      return reject(windowsError("open optical drive for TOC"));
   }
   const auto tracks = layout.getTracks();
   if (tracks.empty()) {
      return reject("expected layout has no tracks");
   }
   CDROM_TOC toc{};
   CDROM_READ_TOC_EX request{};
   request.Format = CDROM_READ_TOC_EX_FORMAT_TOC;
   request.Msf = 1;
   request.SessionTrack = static_cast<UCHAR>(tracks.front().number);
   DWORD returned = 0;
   if (!DeviceIoControl(device.get(), IOCTL_CDROM_READ_TOC_EX, &request, sizeof(request),
                        &toc, sizeof(toc), &returned, nullptr)) {
      return reject(windowsError("IOCTL_CDROM_READ_TOC_EX"));
   }
   if (toc.FirstTrack != tracks.front().number || toc.LastTrack != tracks.back().number ||
       returned < offsetof(CDROM_TOC, TrackData) + (tracks.size() + 1) * sizeof(TRACK_DATA)) {
      return reject("TOC track range/length differs: first=" + std::to_string(toc.FirstTrack) +
                    " last=" + std::to_string(toc.LastTrack) + " bytes=" + std::to_string(returned));
   }
   for (std::size_t position = 0; position < tracks.size(); ++position) {
      const auto& expected = tracks[position];
      const auto& actual = toc.TrackData[position];
      const auto indexOne = std::find_if(expected.indices.begin(), expected.indices.end(), [](const auto& index) {
         return index.number == 1;
      });
      if (actual.TrackNumber != expected.number || (actual.Control & 4) != 0 ||
          ((actual.Control & 1) != 0) != expected.flags.preEmphasis ||
          (expected.flags.digitalCopyPermitted && ((actual.Control & 2) != 0) != *expected.flags.digitalCopyPermitted) ||
          indexOne == expected.indices.end() || tocLba(actual) != indexOne->discLba) {
         return reject("track " + std::to_string(expected.number) + " TOC differs: number=" +
                       std::to_string(actual.TrackNumber) + " control=" + std::to_string(actual.Control) +
                       " actual LBA=" + std::to_string(tocLba(actual)) + " expected INDEX 01=" +
                       (indexOne == expected.indices.end() ? "missing" : std::to_string(indexOne->discLba)));
      }
   }
   const bool matched = toc.TrackData[tracks.size()].TrackNumber == 0xaa &&
          tocLba(toc.TrackData[tracks.size()]) == layout.getLeadOut().discLba;
   tocDiagnostic = matched ? "TOC matches" : "TOC lead-out differs: actual=" +
      std::to_string(tocLba(toc.TrackData[tracks.size()])) + " expected=" + std::to_string(layout.getLeadOut().discLba);
   return matched;
}

std::string WindowsImgDriveHost::layoutDiagnostic() const {
   return tocDiagnostic;
}

std::string WindowsImgDriveHost::sessionIdentity() {
   HANDLE rawToken = nullptr;
   if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &rawToken)) {
      return {};
   }
   WindowsHandle token(rawToken);
   TOKEN_STATISTICS statistics{};
   DWORD statisticsSize = 0;
   if (!GetTokenInformation(token.get(), TokenStatistics, &statistics, sizeof(statistics), &statisticsSize)) {
      return {};
   }
   struct LogonData {
      PSECURITY_LOGON_SESSION_DATA value = nullptr;
      ~LogonData() {
         if (value) {
            LsaFreeReturnBuffer(value);
         }
      }
   } logon;
   if (LsaGetLogonSessionData(&statistics.AuthenticationId, &logon.value) != 0 || !logon.value ||
       logon.value->Size < offsetof(SECURITY_LOGON_SESSION_DATA, LogonTime) + sizeof(LARGE_INTEGER) ||
       logon.value->LogonTime.QuadPart <= 0) {
      return {}; // Unknown identity must never authorize eject-intent removal.
   }
   const auto logonTime = static_cast<std::uint64_t>(logon.value->LogonTime.QuadPart);
   DWORD size = 0;
   GetTokenInformation(token.get(), TokenGroups, nullptr, 0, &size);
   if (size == 0) {
      return {};
   }
   std::vector<std::byte> buffer(size);
   if (!GetTokenInformation(token.get(), TokenGroups, buffer.data(), size, &size)) {
      return {};
   }
   const auto* groups = reinterpret_cast<const TOKEN_GROUPS*>(buffer.data());
   for (DWORD index = 0; index < groups->GroupCount; ++index) {
      if ((groups->Groups[index].Attributes & SE_GROUP_LOGON_ID) == SE_GROUP_LOGON_ID) {
         wchar_t* sid = nullptr;
         if (!ConvertSidToStringSidW(groups->Groups[index].Sid, &sid)) {
            return {};
         }
         const auto identity = pathToUtf8(std::filesystem::path(sid));
         LocalFree(sid);
         return makeSessionIdentity(identity, logonTime);
      }
   }
   return {};
}

std::vector<OpticalDrive> WindowsImgDriveHost::opticalDrives() {
   struct DeviceSet {
      HDEVINFO handle = SetupDiGetClassDevsW(&GUID_DEVINTERFACE_CDROM, nullptr, nullptr,
                                            DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
      ~DeviceSet() {
         if (handle != INVALID_HANDLE_VALUE) {
            SetupDiDestroyDeviceInfoList(handle);
         }
      }
   } devices;
   if (devices.handle == INVALID_HANDLE_VALUE) {
      throw std::runtime_error(windowsError("enumerate optical devices"));
   }
   std::vector<OpticalDrive> result;
   for (char letter = 'D'; letter <= 'Z'; ++letter) {
      if (GetDriveTypeW(rootPath(letter).c_str()) != DRIVE_CDROM) {
         continue;
      }
      OpticalDrive drive;
      drive.letter = letter;
      drive.media = inspectDrive(letter);
      WindowsHandle volume(openDrive(letter, 0));
      STORAGE_DEVICE_NUMBER number{};
      DWORD returned = 0;
      const bool numbered = volume.get() != INVALID_HANDLE_VALUE &&
         DeviceIoControl(volume.get(), IOCTL_STORAGE_GET_DEVICE_NUMBER, nullptr, 0,
                         &number, sizeof(number), &returned, nullptr) && returned >= sizeof(number);
      for (DWORD index = 0; numbered; ++index) {
         SP_DEVICE_INTERFACE_DATA item{};
         item.cbSize = sizeof(item);
         if (!SetupDiEnumDeviceInterfaces(devices.handle, nullptr, &GUID_DEVINTERFACE_CDROM, index, &item)) {
            if (GetLastError() != ERROR_NO_MORE_ITEMS) {
               throw std::runtime_error(windowsError("enumerate optical interface"));
            }
            break;
         }
         DWORD bytes = 0;
         SetupDiGetDeviceInterfaceDetailW(devices.handle, &item, nullptr, 0, &bytes, nullptr);
         if (bytes < sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W)) {
            continue;
         }
         std::vector<std::byte> buffer(bytes);
         auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buffer.data());
         detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
         SP_DEVINFO_DATA info{};
         info.cbSize = sizeof(info);
         if (!SetupDiGetDeviceInterfaceDetailW(devices.handle, &item, detail, bytes, nullptr, &info)) {
            continue;
         }
         WindowsHandle device(CreateFileW(detail->DevicePath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                           nullptr, OPEN_EXISTING, 0, nullptr));
         STORAGE_DEVICE_NUMBER candidate{};
         if (device.get() == INVALID_HANDLE_VALUE ||
             !DeviceIoControl(device.get(), IOCTL_STORAGE_GET_DEVICE_NUMBER, nullptr, 0,
                              &candidate, sizeof(candidate), &returned, nullptr) ||
             returned < sizeof(candidate) || candidate.DeviceType != number.DeviceType ||
             candidate.DeviceNumber != number.DeviceNumber) {
            continue;
         }
         std::array<wchar_t, MAX_DEVICE_ID_LEN> id{};
         if (CM_Get_Device_IDW(info.DevInst, id.data(), static_cast<ULONG>(id.size()), 0) != CR_SUCCESS) {
            continue;
         }
         drive.identity = pathToUtf8(std::filesystem::path(id.data()));
         auto node = info.DevInst;
         for (int depth = 0; depth < 16; ++depth) {
            std::array<wchar_t, 1024> service{};
            ULONG size = static_cast<ULONG>(service.size() * sizeof(wchar_t));
            ULONG type = 0;
            if (CM_Get_DevNode_Registry_PropertyW(node, CM_DRP_SERVICE, &type, service.data(), &size, 0) == CR_SUCCESS &&
                type == REG_SZ && size <= service.size() * sizeof(wchar_t)) {
               service.back() = L'\0';
               drive.description += "service=" + pathToUtf8(std::filesystem::path(service.data())) + " ";
               // Match the actual PnP driver service, not a friendly optical
               // name, physical-drive emptiness, or an assumed SCSI target ID.
               drive.imgDrive = drive.imgDrive || _wcsicmp(service.data(), L"ImgDrive") == 0;
            }
            DEVINST parent = 0;
            if (CM_Get_Parent(&parent, node, 0) != CR_SUCCESS) {
               break;
            }
            node = parent;
         }
         break;
      }
      if (drive.identity.empty()) {
         drive.description = "PnP identity unavailable; selection refused";
      }
      result.push_back(std::move(drive));
   }
   return result;
}

std::optional<std::vector<char>> WindowsImgDriveHost::emptyDrives() {
   std::vector<char> result;
   for (const auto& drive : opticalDrives()) {
      if (drive.imgDrive && !drive.identity.empty() && drive.media == MediaState::Empty) {
         result.push_back(drive.letter);
      }
   }
   return result;
}

std::unique_ptr<MountLock> WindowsImgDriveHost::lock(const std::filesystem::path& cache,
                                                     std::chrono::milliseconds timeout) {
   return std::make_unique<WindowsMountLock>(cache, timeout);
}

void WindowsImgDriveHost::pause(std::chrono::milliseconds duration) {
   std::this_thread::sleep_for(duration);
}

}
