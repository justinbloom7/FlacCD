#include "flaccd/mount/MountFileSystem.h"
#include "flaccd/PathText.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <iterator>
#include <sstream>
#include <stdexcept>

#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace flaccd {
namespace {

#ifdef _WIN32
using FileHandle = HANDLE;
const FileHandle INVALID_FILE = INVALID_HANDLE_VALUE;
std::error_code lastError() {
   return {static_cast<int>(GetLastError()), std::system_category()};
}
FileHandle createFile(const std::filesystem::path& path) {
   return CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
}
bool writeBytes(FileHandle file, const char* data, std::size_t size) {
   DWORD written = 0;
   return WriteFile(file, data, static_cast<DWORD>(size), &written, nullptr) && written == size;
}
bool closeFile(FileHandle file) {
   return CloseHandle(file) != 0;
}
std::string handleIdentity(FileHandle file) {
   BY_HANDLE_FILE_INFORMATION info{};
   if (!GetFileInformationByHandle(file, &info)) {
      throw std::system_error(lastError(), "read file identity");
   }
   std::ostringstream text;
   text << info.dwVolumeSerialNumber << ':' << info.nFileIndexHigh << ':' << info.nFileIndexLow << ':'
        << info.ftCreationTime.dwHighDateTime << ':' << info.ftCreationTime.dwLowDateTime << ':'
        << info.ftLastWriteTime.dwHighDateTime << ':' << info.ftLastWriteTime.dwLowDateTime << ':'
        << info.nFileSizeHigh << ':' << info.nFileSizeLow;
   return text.str();
}
#else
using FileHandle = int;
constexpr FileHandle INVALID_FILE = -1;
std::error_code lastError() {
   return {errno, std::generic_category()};
}
FileHandle createFile(const std::filesystem::path& path) {
   return ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
}
bool writeBytes(FileHandle file, const char* data, std::size_t size) {
   while (size > 0) {
      const auto count = ::write(file, data, size);
      if (count < 0 && errno == EINTR) {
         continue;
      }
      if (count <= 0) {
         return false;
      }
      data += count;
      size -= static_cast<std::size_t>(count);
   }
   return true;
}
bool closeFile(FileHandle file) {
   return ::close(file) == 0;
}
std::string handleIdentity(FileHandle file) {
   struct stat info{};
   if (::fstat(file, &info) != 0 || !S_ISREG(info.st_mode)) {
      throw std::runtime_error("cannot identify regular mount resource");
   }
   std::ostringstream text;
   struct statx extended{};
   const bool hasBirth = ::statx(file, "", AT_EMPTY_PATH, STATX_BTIME, &extended) == 0 &&
                         (extended.stx_mask & STATX_BTIME) != 0;
   text << info.st_dev << ':' << info.st_ino << ':' << (hasBirth ? extended.stx_btime.tv_sec : 0) << ':'
        << (hasBirth ? extended.stx_btime.tv_nsec : 0) << ':' << info.st_mtim.tv_sec << ':' << info.st_mtim.tv_nsec
        << ':' << info.st_size;
   return text.str();
}
#endif

class File {
public:
   explicit File(FileHandle value) : handle(value) {
   }
   ~File() {
      if (handle != INVALID_FILE) {
         closeFile(handle);
      }
   }
   File(const File&) = delete;
   File& operator=(const File&) = delete;
   bool close() {
      const auto value = handle;
      handle = INVALID_FILE;
      return closeFile(value);
   }
   FileHandle handle;
};

bool regular(const std::filesystem::path& path) {
   return std::filesystem::is_regular_file(std::filesystem::symlink_status(path));
}

}

bool mountPathExists(const std::filesystem::path& path) {
   return std::filesystem::exists(std::filesystem::symlink_status(path));
}

std::string readMountCue(const std::filesystem::path& path) {
   constexpr std::uintmax_t MAX_CUE_BYTES = 1024 * 1024;
   if (!regular(path) || std::filesystem::file_size(path) > MAX_CUE_BYTES) {
      throw std::runtime_error("unsafe or oversized mount metadata: " + pathToUtf8(path));
   }
   std::ifstream input(path, std::ios::binary);
   if (!input) {
      throw std::runtime_error("cannot read mount metadata: " + pathToUtf8(path));
   }
   std::string text{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
   if (input.bad()) {
      throw std::runtime_error("failed reading mount metadata");
   }
   return text;
}

bool expectedLinkFailure(const std::error_code& error) {
#ifdef _WIN32
   // libstdc++ versions differ in their Win32-to-errc mappings. Recognize
   // documented filesystem limitations without treating arbitrary I/O errors
   // (for example a damaged disk) as permission to start a large copy.
   if (error.category() == std::system_category()) {
      switch (error.value()) {
      case ERROR_INVALID_FUNCTION:
      case ERROR_NOT_SUPPORTED:
      case ERROR_CALL_NOT_IMPLEMENTED:
      case ERROR_NOT_SAME_DEVICE:
      case ERROR_ACCESS_DENIED:
      case ERROR_PRIVILEGE_NOT_HELD:
      case ERROR_WRITE_PROTECT:
      case ERROR_TOO_MANY_LINKS:
         return true;
      }
   }
#endif
   return error == std::errc::cross_device_link || error == std::errc::function_not_supported ||
          error == std::errc::operation_not_supported || error == std::errc::not_supported ||
          error == std::errc::operation_not_permitted || error == std::errc::permission_denied ||
          error == std::errc::too_many_links || error == std::errc::read_only_file_system;
}

std::error_code MountFileSystem::createCueExclusive(const std::filesystem::path& path, const std::string& text) {
   File file(createFile(path));
   if (file.handle == INVALID_FILE) {
      const auto error = lastError();
#ifdef _WIN32
      if (error.value() == ERROR_FILE_EXISTS || error.value() == ERROR_ALREADY_EXISTS) {
         return std::make_error_code(std::errc::file_exists);
      }
#endif
      return error;
   }
   if (!writeBytes(file.handle, text.data(), text.size()) || !file.close()) {
      throw std::runtime_error("failed writing exclusively created CUE; preserve it for inspection: " + pathToUtf8(path));
   }
   return {};
}

std::error_code MountFileSystem::hardLink(const std::filesystem::path& source,
                                          const std::filesystem::path& destination) {
   std::error_code error;
   std::filesystem::create_hard_link(source, destination, error);
   return error;
}

std::string MountFileSystem::identity(const std::filesystem::path& path) {
   if (!regular(path)) {
      throw std::runtime_error("mount resource is missing, nonregular, or a symlink: " + pathToUtf8(path));
   }
#ifdef _WIN32
   File file(CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
#else
   File file(::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
#endif
   if (file.handle == INVALID_FILE) {
      throw std::system_error(lastError(), "open mount resource for identity");
   }
   return handleIdentity(file.handle);
}

void MountFileSystem::copyCompressed(const std::filesystem::path& source,
                                     const std::filesystem::path& destination,
                                     const MountLog& log) {
   const auto bytes = std::filesystem::file_size(source);
   std::error_code spaceError;
   const auto space = std::filesystem::space(destination.parent_path(), spaceError);
   if (!spaceError && space.available < bytes) {
      throw std::runtime_error("insufficient cache space for compressed FLAC copy");
   }
   if (log) {
      log("Copying compressed FLAC (" + std::to_string(bytes) + " bytes): " + pathToUtf8(source) + " -> " +
          pathToUtf8(destination));
   }
   const auto before = identity(source);
   std::ifstream input(source, std::ios::binary);
   if (!input) {
      throw std::runtime_error("cannot open FLAC for compressed copy");
   }
   File output(createFile(destination));
   if (output.handle == INVALID_FILE) {
      throw std::system_error(lastError(), "exclusive compressed FLAC copy");
   }
   std::array<char, 128 * 1024> buffer;
   std::uintmax_t copied = 0;
   while (input) {
      input.read(buffer.data(), buffer.size());
      const auto count = static_cast<std::size_t>(input.gcount());
      if (count > 0 && !writeBytes(output.handle, buffer.data(), count)) {
         throw std::runtime_error("compressed FLAC copy write failed; incomplete staging retained, never mounted");
      }
      copied += count;
   }
   if (input.bad() || copied != bytes || !output.close() || identity(source) != before) {
      throw std::runtime_error("compressed FLAC copy incomplete or source changed; staging retained, never mounted");
   }
   // Bounded second pass verifies exact encoded bytes, independent of decoding.
   input.clear();
   input.seekg(0);
   std::ifstream copy(destination, std::ios::binary);
   std::array<char, 128 * 1024> other;
   while (input) {
      input.read(buffer.data(), buffer.size());
      const auto count = input.gcount();
      copy.read(other.data(), count);
      if (copy.gcount() != count || !std::equal(buffer.begin(), buffer.begin() + count, other.begin())) {
         throw std::runtime_error("compressed FLAC copy verification failed");
      }
   }
   if (input.bad() || copy.bad() || identity(source) != before) {
      throw std::runtime_error("compressed FLAC verification failed or source changed");
   }
}

void MountFileSystem::removeOwned(const std::filesystem::path& path,
                                  const std::string& expectedIdentity,
                                  const std::string* expectedContents) {
   if (!mountPathExists(path)) {
      return;
   }
   if (identity(path) != expectedIdentity || (expectedContents && readMountCue(path) != *expectedContents)) {
      throw std::runtime_error("owned resource was replaced or modified; preserved: " + pathToUtf8(path));
   }
#ifdef _WIN32
   // Delete the verified file by handle, preventing a pathname replacement
   // between identity verification and deletion. Exclude concurrent writes/deletes.
   File file(CreateFileW(path.c_str(), GENERIC_READ | DELETE, FILE_SHARE_READ,
                         nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
   if (file.handle == INVALID_FILE || handleIdentity(file.handle) != expectedIdentity) {
      throw std::runtime_error("resource changed or is busy; preserved: " + pathToUtf8(path));
   }
   FILE_DISPOSITION_INFO disposition{TRUE};
   if (!SetFileInformationByHandle(file.handle, FileDispositionInfo, &disposition, sizeof(disposition))) {
      throw std::system_error(lastError(), "delete verified owned mount resource");
   }
#else
   // FlacCD serializes cooperating requests with the cache lock. Recheck identity
   // immediately before unlink; arbitrary concurrent directory tampering is not supported.
   if (identity(path) != expectedIdentity) {
      throw std::runtime_error("resource changed before cleanup; preserved");
   }
   std::filesystem::remove(path);
#endif
}

}
