#pragma once

#include "wt/core/Status.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

typedef struct _LIBSSH2_SFTP LIBSSH2_SFTP;

namespace wt::ssh {

class SshConnection;

struct SftpEntry {
    std::string name;
    std::uint64_t size = 0;
    std::int64_t modified = 0; // seconds since the epoch
    std::uint32_t permissions = 0;
    bool isDirectory = false;
    bool isSymlink = false;
};

// Bytes done / total; return false to cancel the transfer.
using TransferProgress = std::function<bool(std::uint64_t done, std::uint64_t total)>;

// SFTP v3 file operations over a blocking connection. Use from one thread.
class SftpSession {
public:
    ~SftpSession();
    SftpSession(const SftpSession&) = delete;
    SftpSession& operator=(const SftpSession&) = delete;

    core::Status list(const std::string& path, std::vector<SftpEntry>& entries); // sorted: dirs first
    core::Status stat(const std::string& path, SftpEntry& entry);
    core::Status realPath(const std::string& path, std::string& resolved);
    core::Status makeDirectory(const std::string& path, std::uint32_t mode = 0755);
    core::Status removeDirectory(const std::string& path);
    core::Status removeFile(const std::string& path);
    core::Status rename(const std::string& from, const std::string& to, bool overwrite = false);
    // Copies a remote file to `localPath` (written via a temporary file, renamed when complete).
    core::Status download(const std::string& remotePath, const std::string& localPath,
                          const TransferProgress& progress = {});
    // Copies a local file to `remotePath` (created/truncated with mode 0644).
    core::Status upload(const std::string& localPath, const std::string& remotePath,
                        const TransferProgress& progress = {});
    // Whole directories: regular files and subdirectories (symbolic links are skipped, so a link
    // to a parent cannot loop). Existing directories are reused, files overwritten. Progress
    // counts the bytes of the whole tree.
    core::Status uploadTree(const std::string& localDirectory, const std::string& remoteDirectory,
                            const TransferProgress& progress = {});
    core::Status downloadTree(const std::string& remoteDirectory, const std::string& localDirectory,
                              const TransferProgress& progress = {});

private:
    friend class SshConnection;
    SftpSession(std::shared_ptr<SshConnection> connection, LIBSSH2_SFTP* sftp);
    core::Status failure(const std::string& what, const std::string& path) const;

    std::shared_ptr<SshConnection> m_connection;
    LIBSSH2_SFTP* m_sftp;
};

// Joins remote path components with '/'.
std::string joinRemotePath(const std::string& directory, const std::string& name);
std::string remoteParent(const std::string& path);

} // namespace wt::ssh
