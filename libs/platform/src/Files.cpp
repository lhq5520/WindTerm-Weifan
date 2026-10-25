#include "wt/platform/Files.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <random>
#include <sstream>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace wt::platform {

std::filesystem::path pathFromUtf8(std::string_view utf8) {
    std::u8string text(utf8.size(), u8'\0');
    std::memcpy(text.data(), utf8.data(), utf8.size());
    return std::filesystem::path(text);
}

std::string pathToUtf8(const std::filesystem::path& path) {
    const std::u8string text = path.u8string();
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

std::optional<std::string> readFile(std::string_view path, std::string* error) {
    std::ifstream in(pathFromUtf8(path), std::ios::binary);
    if (!in) {
        if (error) {
            *error = "cannot open " + std::string(path) + ": " + std::strerror(errno);
        }
        return std::nullopt;
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    if (in.bad()) {
        if (error) {
            *error = "cannot read " + std::string(path);
        }
        return std::nullopt;
    }
    return std::move(buffer).str();
}

core::Status writeFileAtomic(std::string_view path, std::string_view data, bool privateFile) {
    const std::filesystem::path target = pathFromUtf8(path);
    std::error_code ec;
    if (target.has_parent_path()) {
        std::filesystem::create_directories(target.parent_path(), ec);
        if (ec) {
            return core::Status::error("cannot create directory " + pathToUtf8(target.parent_path()) + ": " +
                                       ec.message());
        }
    }
    std::random_device random;
    std::filesystem::path temp = target;
    temp += ".tmp" + std::to_string(random());

#ifdef _WIN32
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out) {
            return core::Status::error("cannot create " + pathToUtf8(temp));
        }
        out.write(data.data(), static_cast<std::streamsize>(data.size()));
        out.flush();
        if (!out) {
            std::filesystem::remove(temp, ec);
            return core::Status::error("cannot write " + pathToUtf8(temp));
        }
    }
    (void)privateFile; // per-user profile directories are private by default ACLs
    if (!MoveFileExW(temp.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const auto code = static_cast<int>(GetLastError());
        std::filesystem::remove(temp, ec);
        return core::Status::error("cannot replace " + pathToUtf8(target), code);
    }
#else
    const int fd = ::open(temp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, privateFile ? 0600 : 0644);
    if (fd < 0) {
        return core::Status::error("cannot create " + temp.string() + ": " + std::strerror(errno), errno);
    }
    std::size_t written = 0;
    while (written < data.size()) {
        const ssize_t n = ::write(fd, data.data() + written, data.size() - written);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            const int err = errno;
            ::close(fd);
            ::unlink(temp.c_str());
            return core::Status::error("cannot write " + temp.string() + ": " + std::strerror(err), err);
        }
        written += static_cast<std::size_t>(n);
    }
    if (::fsync(fd) != 0 || ::close(fd) != 0) {
        const int err = errno;
        ::unlink(temp.c_str());
        return core::Status::error("cannot flush " + temp.string() + ": " + std::strerror(err), err);
    }
    if (::rename(temp.c_str(), target.c_str()) != 0) {
        const int err = errno;
        ::unlink(temp.c_str());
        return core::Status::error("cannot replace " + target.string() + ": " + std::strerror(err), err);
    }
#endif
    return core::Status::ok();
}

core::Status appendToFile(std::string_view path, std::string_view data, bool privateIfNew) {
    const std::filesystem::path target = pathFromUtf8(path);
    std::error_code ec;
    if (target.has_parent_path()) {
        std::filesystem::create_directories(target.parent_path(), ec);
    }
#ifdef _WIN32
    (void)privateIfNew;
    std::ofstream out(target, std::ios::binary | std::ios::app);
    if (!out) {
        return core::Status::error("cannot open " + pathToUtf8(target) + " for appending");
    }
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
    out.flush();
    return out ? core::Status::ok() : core::Status::error("cannot write " + pathToUtf8(target));
#else
    const int fd = ::open(target.c_str(), O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, privateIfNew ? 0600 : 0644);
    if (fd < 0) {
        return core::Status::error("cannot open " + target.string() + ": " + std::strerror(errno), errno);
    }
    std::size_t written = 0;
    while (written < data.size()) {
        const ssize_t n = ::write(fd, data.data() + written, data.size() - written);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            const int err = errno;
            ::close(fd);
            return core::Status::error("cannot write " + target.string() + ": " + std::strerror(err), err);
        }
        written += static_cast<std::size_t>(n);
    }
    ::fsync(fd);
    ::close(fd);
    return core::Status::ok();
#endif
}

std::string homeDirectory() {
#ifdef _WIN32
    if (const wchar_t* profile = _wgetenv(L"USERPROFILE"); profile && *profile) {
        return pathToUtf8(std::filesystem::path(profile));
    }
    return {};
#else
    if (const char* home = std::getenv("HOME"); home && *home) {
        return home;
    }
    if (const passwd* pw = ::getpwuid(::getuid()); pw && pw->pw_dir) {
        return pw->pw_dir;
    }
    return {};
#endif
}

} // namespace wt::platform
