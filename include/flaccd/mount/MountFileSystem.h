#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <system_error>

namespace flaccd {

using MountLog = std::function<void(const std::string& message)>;

// Native filesystem operations are isolated here for capability/race injection
// in Linux tests. Identity tokens include file identity and modification data.
class MountFileSystem {
public:
   virtual ~MountFileSystem() = default;
   virtual std::error_code createCueExclusive(const std::filesystem::path& path, const std::string& text);
   virtual std::error_code hardLink(const std::filesystem::path& source, const std::filesystem::path& destination);
   virtual void copyCompressed(const std::filesystem::path& source,
                               const std::filesystem::path& destination,
                               const MountLog& log);
   virtual std::string identity(const std::filesystem::path& path);
   virtual void removeOwned(const std::filesystem::path& path,
                            const std::string& expectedIdentity,
                            const std::string* expectedContents = nullptr);
};

std::string readMountCue(const std::filesystem::path& path);
bool mountPathExists(const std::filesystem::path& path);
bool expectedLinkFailure(const std::error_code& error);
// Same-filesystem atomic publication, used only while holding the cache lock.
void publishMountMetadata(const std::filesystem::path& temporary, const std::filesystem::path& destination);

}
