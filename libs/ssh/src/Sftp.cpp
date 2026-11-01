#include "wt/ssh/Sftp.h"

#include "Libssh2Init.h"

#include "wt/platform/Files.h"
#include "wt/ssh/SshConnection.h"

#include <libssh2_sftp.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>

namespace wt::ssh {
namespace {

constexpr std::size_t kChunk = 256 * 1024;

std::string sftpErrorText(unsigned long code) {
    switch (code) {
    case LIBSSH2_FX_NO_SUCH_FILE: return "no such file";
    case LIBSSH2_FX_PERMISSION_DENIED: return "permission denied";
    case LIBSSH2_FX_FAILURE: return "failure";
    case LIBSSH2_FX_NO_SUCH_PATH: return "no such path";
    case LIBSSH2_FX_FILE_ALREADY_EXISTS: return "file already exists";
    case LIBSSH2_FX_WRITE_PROTECT: return "write protected";
    case LIBSSH2_FX_NO_SPACE_ON_FILESYSTEM: return "no space left";
    case LIBSSH2_FX_QUOTA_EXCEEDED: return "quota exceeded";
    case LIBSSH2_FX_DIR_NOT_EMPTY: return "directory not empty";
    case LIBSSH2_FX_NOT_A_DIRECTORY: return "not a directory";
    case LIBSSH2_FX_INVALID_FILENAME: return "invalid file name";
    case LIBSSH2_FX_LINK_LOOP: return "too many symbolic links";
    default: return "SFTP error " + std::to_string(code);
    }
}

SftpEntry entryFrom(const std::string& name, const LIBSSH2_SFTP_ATTRIBUTES& attrs) {
    SftpEntry entry;
    entry.name = name;
    if (attrs.flags & LIBSSH2_SFTP_ATTR_SIZE) {
        entry.size = attrs.filesize;
    }
    if (attrs.flags & LIBSSH2_SFTP_ATTR_ACMODTIME) {
        entry.modified = static_cast<std::int64_t>(attrs.mtime);
    }
    if (attrs.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS) {
        entry.permissions = static_cast<std::uint32_t>(attrs.permissions);
        entry.isDirectory = LIBSSH2_SFTP_S_ISDIR(attrs.permissions);
        entry.isSymlink = LIBSSH2_SFTP_S_ISLNK(attrs.permissions);
    }
    return entry;
}

} // namespace

std::string joinRemotePath(const std::string& directory, const std::string& name) {
    if (directory.empty()) {
        return name;
    }
    return directory.back() == '/' ? directory + name : directory + "/" + name;
}

std::string remoteParent(const std::string& path) {
    if (path.empty() || path == "/") {
        return "/";
    }
    std::string trimmed = path;
    while (trimmed.size() > 1 && trimmed.back() == '/') {
        trimmed.pop_back();
    }
    const auto slash = trimmed.rfind('/');
    if (slash == std::string::npos) {
        return ".";
    }
    return slash == 0 ? "/" : trimmed.substr(0, slash);
}

SftpSession::SftpSession(std::shared_ptr<SshConnection> connection, LIBSSH2_SFTP* sftp)
    : m_connection(std::move(connection)), m_sftp(sftp) {}

SftpSession::~SftpSession() {
    if (m_sftp) {
        libssh2_sftp_shutdown(m_sftp);
    }
}

core::Status SftpSession::failure(const std::string& what, const std::string& path) const {
    const unsigned long code = libssh2_sftp_last_error(m_sftp);
    const std::string reason = code != LIBSSH2_FX_OK ? sftpErrorText(code) : libssh2Error(m_connection->session());
    return core::Status::error(what + " " + path + ": " + reason, static_cast<int>(code));
}

core::Status SftpSession::list(const std::string& path, std::vector<SftpEntry>& entries) {
    entries.clear();
    LIBSSH2_SFTP_HANDLE* dir = libssh2_sftp_opendir(m_sftp, path.c_str());
    if (!dir) {
        return failure("cannot open directory", path);
    }
    char name[1024];
    char longEntry[2048];
    LIBSSH2_SFTP_ATTRIBUTES attrs{};
    for (;;) {
        const int rc = libssh2_sftp_readdir_ex(dir, name, sizeof(name), longEntry, sizeof(longEntry), &attrs);
        if (rc <= 0) {
            if (rc < 0) {
                libssh2_sftp_closedir(dir);
                return failure("cannot read directory", path);
            }
            break;
        }
        const std::string entryName(name, static_cast<std::size_t>(rc));
        if (entryName == "." || entryName == "..") {
            continue;
        }
        SftpEntry entry = entryFrom(entryName, attrs);
        if (entry.isSymlink) {
            // Follow links so directories behave like directories.
            LIBSSH2_SFTP_ATTRIBUTES target{};
            const std::string full = joinRemotePath(path, entryName);
            if (libssh2_sftp_stat_ex(m_sftp, full.c_str(), static_cast<unsigned>(full.size()), LIBSSH2_SFTP_STAT,
                                     &target) == 0 &&
                (target.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS)) {
                entry.isDirectory = LIBSSH2_SFTP_S_ISDIR(target.permissions);
            }
        }
        entries.push_back(std::move(entry));
    }
    libssh2_sftp_closedir(dir);
    std::sort(entries.begin(), entries.end(), [](const SftpEntry& a, const SftpEntry& b) {
        if (a.isDirectory != b.isDirectory) {
            return a.isDirectory;
        }
        return a.name < b.name;
    });
    return core::Status::ok();
}

core::Status SftpSession::stat(const std::string& path, SftpEntry& entry) {
    LIBSSH2_SFTP_ATTRIBUTES attrs{};
    if (libssh2_sftp_stat_ex(m_sftp, path.c_str(), static_cast<unsigned>(path.size()), LIBSSH2_SFTP_STAT, &attrs) != 0) {
        return failure("cannot stat", path);
    }
    entry = entryFrom(path.substr(path.rfind('/') == std::string::npos ? 0 : path.rfind('/') + 1), attrs);
    return core::Status::ok();
}

core::Status SftpSession::realPath(const std::string& path, std::string& resolved) {
    char buffer[4096];
    const int rc = libssh2_sftp_realpath(m_sftp, path.c_str(), buffer, sizeof(buffer));
    if (rc < 0) {
        return failure("cannot resolve", path);
    }
    resolved.assign(buffer, static_cast<std::size_t>(rc));
    return core::Status::ok();
}

core::Status SftpSession::makeDirectory(const std::string& path, std::uint32_t mode) {
    if (libssh2_sftp_mkdir_ex(m_sftp, path.c_str(), static_cast<unsigned>(path.size()), static_cast<long>(mode)) != 0) {
        return failure("cannot create directory", path);
    }
    return core::Status::ok();
}

core::Status SftpSession::removeDirectory(const std::string& path) {
    if (libssh2_sftp_rmdir_ex(m_sftp, path.c_str(), static_cast<unsigned>(path.size())) != 0) {
        return failure("cannot remove directory", path);
    }
    return core::Status::ok();
}

core::Status SftpSession::removeFile(const std::string& path) {
    if (libssh2_sftp_unlink_ex(m_sftp, path.c_str(), static_cast<unsigned>(path.size())) != 0) {
        return failure("cannot remove", path);
    }
    return core::Status::ok();
}

core::Status SftpSession::rename(const std::string& from, const std::string& to, bool overwrite) {
    long flags = LIBSSH2_SFTP_RENAME_ATOMIC | LIBSSH2_SFTP_RENAME_NATIVE;
    if (overwrite) {
        flags |= LIBSSH2_SFTP_RENAME_OVERWRITE;
    }
    if (libssh2_sftp_rename_ex(m_sftp, from.c_str(), static_cast<unsigned>(from.size()), to.c_str(),
                               static_cast<unsigned>(to.size()), flags) != 0) {
        return failure("cannot rename", from);
    }
    return core::Status::ok();
}

core::Status SftpSession::download(const std::string& remotePath, const std::string& localPath,
                                   const TransferProgress& progress) {
    SftpEntry info;
    if (auto status = stat(remotePath, info); !status) {
        return status;
    }
    if (info.isDirectory) {
        return core::Status::error("cannot download " + remotePath + ": it is a directory");
    }
    LIBSSH2_SFTP_HANDLE* file = libssh2_sftp_open_ex(m_sftp, remotePath.c_str(), static_cast<unsigned>(remotePath.size()),
                                                     LIBSSH2_FXF_READ, 0, LIBSSH2_SFTP_OPENFILE);
    if (!file) {
        return failure("cannot open", remotePath);
    }
    const std::filesystem::path target = platform::pathFromUtf8(localPath);
    std::filesystem::path temp = target;
    temp += ".part" + std::to_string(std::random_device{}() % 100000);
    std::ofstream out(temp, std::ios::binary | std::ios::trunc);
    if (!out) {
        libssh2_sftp_close(file);
        return core::Status::error("cannot write " + localPath);
    }
    std::vector<char> buffer(kChunk);
    std::uint64_t done = 0;
    core::Status status = core::Status::ok();
    for (;;) {
        const auto n = libssh2_sftp_read(file, buffer.data(), buffer.size());
        if (n < 0) {
            status = failure("cannot read", remotePath);
            break;
        }
        if (n == 0) {
            break;
        }
        out.write(buffer.data(), n);
        if (!out) {
            status = core::Status::error("cannot write " + localPath);
            break;
        }
        done += static_cast<std::uint64_t>(n);
        if (progress && !progress(done, info.size)) {
            status = core::Status::error("transfer cancelled");
            break;
        }
    }
    libssh2_sftp_close(file);
    out.close();
    std::error_code ec;
    if (!status) {
        std::filesystem::remove(temp, ec);
        return status;
    }
    std::filesystem::rename(temp, target, ec);
    if (ec) {
        std::filesystem::remove(temp, ec);
        return core::Status::error("cannot replace " + localPath + ": " + ec.message());
    }
    return core::Status::ok();
}

core::Status SftpSession::upload(const std::string& localPath, const std::string& remotePath,
                                 const TransferProgress& progress) {
    std::ifstream in(platform::pathFromUtf8(localPath), std::ios::binary);
    if (!in) {
        return core::Status::error("cannot read " + localPath);
    }
    std::error_code ec;
    const std::uint64_t total = std::filesystem::file_size(platform::pathFromUtf8(localPath), ec);
    LIBSSH2_SFTP_HANDLE* file = libssh2_sftp_open_ex(
        m_sftp, remotePath.c_str(), static_cast<unsigned>(remotePath.size()),
        LIBSSH2_FXF_WRITE | LIBSSH2_FXF_CREAT | LIBSSH2_FXF_TRUNC,
        LIBSSH2_SFTP_S_IRUSR | LIBSSH2_SFTP_S_IWUSR | LIBSSH2_SFTP_S_IRGRP | LIBSSH2_SFTP_S_IROTH, LIBSSH2_SFTP_OPENFILE);
    if (!file) {
        return failure("cannot create", remotePath);
    }
    std::vector<char> buffer(kChunk);
    std::uint64_t done = 0;
    core::Status status = core::Status::ok();
    while (status && in) {
        in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = static_cast<std::size_t>(in.gcount());
        if (count == 0) {
            break;
        }
        std::size_t written = 0;
        while (written < count) {
            const auto n = libssh2_sftp_write(file, buffer.data() + written, count - written);
            if (n < 0) {
                status = failure("cannot write", remotePath);
                break;
            }
            written += static_cast<std::size_t>(n);
        }
        done += written;
        if (status && progress && !progress(done, total)) {
            status = core::Status::error("transfer cancelled");
        }
    }
    if (libssh2_sftp_close(file) != 0 && status) {
        status = failure("cannot finish writing", remotePath);
    }
    return status;
}

namespace {

// Adds the bytes of earlier files to a per-file progress callback.
TransferProgress offsetProgress(const TransferProgress& progress, std::uint64_t base, std::uint64_t total) {
    if (!progress) {
        return {};
    }
    return [progress, base, total](std::uint64_t done, std::uint64_t) { return progress(base + done, total); };
}

} // namespace

core::Status SftpSession::uploadTree(const std::string& localDirectory, const std::string& remoteDirectory,
                                     const TransferProgress& progress) {
    namespace fs = std::filesystem;
    const fs::path root = platform::pathFromUtf8(localDirectory);
    std::error_code ec;
    if (!fs::is_directory(root, ec)) {
        return core::Status::error(localDirectory + " is not a directory");
    }
    // Collect first: the total size drives the progress, and nothing is created on errors here.
    struct Item {
        std::string relative; // with '/' separators
        bool directory;
        std::uint64_t size;
    };
    std::vector<Item> items;
    std::uint64_t total = 0;
    for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::none, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        const fs::directory_entry& entry = *it;
        if (entry.is_symlink(ec)) {
            if (entry.is_directory(ec)) {
                it.disable_recursion_pending();
            }
            continue;
        }
        std::string relative = platform::pathToUtf8(fs::relative(entry.path(), root, ec));
        std::replace(relative.begin(), relative.end(), '\\', '/');
        if (entry.is_directory(ec)) {
            items.push_back({relative, true, 0});
        } else if (entry.is_regular_file(ec)) {
            const auto size = entry.file_size(ec);
            items.push_back({relative, false, ec ? 0 : size});
            total += ec ? 0 : size;
        }
    }
    if (ec) {
        return core::Status::error("cannot read " + localDirectory + ": " + ec.message());
    }
    auto ensureDirectory = [this](const std::string& path) {
        SftpEntry existing;
        if (stat(path, existing) && existing.isDirectory) {
            return core::Status::ok();
        }
        return makeDirectory(path);
    };
    if (auto status = ensureDirectory(remoteDirectory); !status) {
        return status;
    }
    std::uint64_t done = 0;
    for (const Item& item : items) {
        const std::string remote = joinRemotePath(remoteDirectory, item.relative);
        if (item.directory) {
            if (auto status = ensureDirectory(remote); !status) {
                return status;
            }
            continue;
        }
        const std::string local = platform::pathToUtf8(root / platform::pathFromUtf8(item.relative));
        if (auto status = upload(local, remote, offsetProgress(progress, done, total)); !status) {
            return status;
        }
        done += item.size;
        if (progress && !progress(done, total)) {
            return core::Status::error("transfer cancelled");
        }
    }
    return core::Status::ok();
}

core::Status SftpSession::downloadTree(const std::string& remoteDirectory, const std::string& localDirectory,
                                       const TransferProgress& progress) {
    namespace fs = std::filesystem;
    // Walk the remote tree first (breadth first, links skipped).
    struct Item {
        std::string relative;
        bool directory;
        std::uint64_t size;
    };
    std::vector<Item> items;
    std::uint64_t total = 0;
    std::vector<std::string> pending{""};
    while (!pending.empty()) {
        const std::string relative = pending.back();
        pending.pop_back();
        std::vector<SftpEntry> entries;
        if (auto status = list(relative.empty() ? remoteDirectory : joinRemotePath(remoteDirectory, relative), entries);
            !status) {
            return status;
        }
        for (const SftpEntry& entry : entries) {
            if (entry.name == "." || entry.name == ".." || entry.isSymlink) {
                continue;
            }
            const std::string child = relative.empty() ? entry.name : relative + "/" + entry.name;
            items.push_back({child, entry.isDirectory, entry.size});
            if (entry.isDirectory) {
                pending.push_back(child);
            } else {
                total += entry.size;
            }
        }
    }
    const fs::path root = platform::pathFromUtf8(localDirectory);
    std::error_code ec;
    fs::create_directories(root, ec);
    if (ec) {
        return core::Status::error("cannot create " + localDirectory + ": " + ec.message());
    }
    // Directories before the files inside them.
    std::stable_sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.directory && !b.directory; });
    std::uint64_t done = 0;
    for (const Item& item : items) {
        const fs::path local = root / platform::pathFromUtf8(item.relative);
        if (item.directory) {
            fs::create_directories(local, ec);
            if (ec) {
                return core::Status::error("cannot create " + platform::pathToUtf8(local) + ": " + ec.message());
            }
            continue;
        }
        if (auto status = download(joinRemotePath(remoteDirectory, item.relative), platform::pathToUtf8(local),
                                   offsetProgress(progress, done, total));
            !status) {
            return status;
        }
        done += item.size;
        if (progress && !progress(done, total)) {
            return core::Status::error("transfer cancelled");
        }
    }
    return core::Status::ok();
}

} // namespace wt::ssh
