#include "wt/ssh/KnownHosts.h"

#include "Libssh2Init.h"

#include "wt/core/Base64.h"
#include "wt/platform/Files.h"

#include <cstring>
#include <sstream>

namespace wt::ssh {
namespace {

int knownHostKeyMask(int hostKeyType) {
    switch (hostKeyType) {
    case LIBSSH2_HOSTKEY_TYPE_RSA: return LIBSSH2_KNOWNHOST_KEY_SSHRSA;
    case LIBSSH2_HOSTKEY_TYPE_DSS: return LIBSSH2_KNOWNHOST_KEY_SSHDSS;
    case LIBSSH2_HOSTKEY_TYPE_ECDSA_256: return LIBSSH2_KNOWNHOST_KEY_ECDSA_256;
    case LIBSSH2_HOSTKEY_TYPE_ECDSA_384: return LIBSSH2_KNOWNHOST_KEY_ECDSA_384;
    case LIBSSH2_HOSTKEY_TYPE_ECDSA_521: return LIBSSH2_KNOWNHOST_KEY_ECDSA_521;
    case LIBSSH2_HOSTKEY_TYPE_ED25519: return LIBSSH2_KNOWNHOST_KEY_ED25519;
    default: return LIBSSH2_KNOWNHOST_KEY_UNKNOWN;
    }
}

std::string hostEntryName(const std::string& host, std::uint16_t port) {
    return port == 22 ? host : "[" + host + "]:" + std::to_string(port);
}

HostKeyCheck checkIn(LIBSSH2_KNOWNHOSTS* hosts, const std::string& host, std::uint16_t port, const HostKey& key) {
    if (!hosts || key.blob.empty()) {
        return HostKeyCheck::Failure;
    }
    libssh2_knownhost* found = nullptr;
    const int rc = libssh2_knownhost_checkp(hosts, host.c_str(), port, key.blob.data(), key.blob.size(),
                                            LIBSSH2_KNOWNHOST_TYPE_PLAIN | LIBSSH2_KNOWNHOST_KEYENC_RAW |
                                                knownHostKeyMask(key.libssh2Type),
                                            &found);
    switch (rc) {
    case LIBSSH2_KNOWNHOST_CHECK_MATCH: return HostKeyCheck::Match;
    case LIBSSH2_KNOWNHOST_CHECK_MISMATCH: return HostKeyCheck::Mismatch;
    case LIBSSH2_KNOWNHOST_CHECK_NOTFOUND: return HostKeyCheck::NotFound;
    default: return HostKeyCheck::Failure;
    }
}

} // namespace

struct KnownHosts::Impl {
    DetachedSession owner;
    LIBSSH2_KNOWNHOSTS* hosts = nullptr;
};

KnownHosts::KnownHosts() : m(std::make_unique<Impl>()) {
    if (m->owner.session) {
        m->hosts = libssh2_knownhost_init(m->owner.session);
    }
}

KnownHosts::~KnownHosts() {
    if (m->hosts) {
        libssh2_knownhost_free(m->hosts);
    }
}

core::Status KnownHosts::load(const std::string& path) {
    if (!m->hosts) {
        return core::Status::error("libssh2 known_hosts support is unavailable");
    }
    std::string error;
    const auto contents = platform::readFile(path, &error);
    if (!contents) {
        std::error_code ec;
        if (!std::filesystem::exists(platform::pathFromUtf8(path), ec)) {
            return core::Status::ok();
        }
        return core::Status::error(error);
    }
    // Feed line by line so one unsupported entry does not stop the rest from loading.
    std::istringstream lines(*contents);
    std::string line;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty() || line[0] == '#') {
            continue;
        }
        libssh2_knownhost_readline(m->hosts, line.c_str(), line.size(), LIBSSH2_KNOWNHOST_FILE_OPENSSH);
    }
    return core::Status::ok();
}

HostKeyCheck KnownHosts::check(const std::string& host, std::uint16_t port, const HostKey& key) const {
    return checkIn(m->hosts, host, port, key);
}

std::string KnownHosts::formatLine(const std::string& host, std::uint16_t port, const HostKey& key) {
    return hostEntryName(host, port) + " " + key.type + " " + core::base64Encode(key.blob);
}

core::Status KnownHosts::append(const std::string& path, const std::string& host, std::uint16_t port,
                                const HostKey& key) {
    std::error_code ec;
    const bool exists = std::filesystem::exists(platform::pathFromUtf8(path), ec);
    std::string prefix;
    if (exists) {
        std::string error;
        const auto contents = platform::readFile(path, &error);
        if (!contents) {
            return core::Status::error(error); // never risk rewriting a file we cannot read
        }
        if (!contents->empty() && contents->back() != '\n') {
            prefix = "\n";
        }
    }
    const std::string line = formatLine(host, port, key);
    if (auto status = platform::appendToFile(path, prefix + line + "\n", true); !status) {
        return status;
    }
    if (m->hosts) {
        libssh2_knownhost_readline(m->hosts, line.c_str(), line.size(), LIBSSH2_KNOWNHOST_FILE_OPENSSH);
    }
    return core::Status::ok();
}

core::Status KnownHosts::replace(const std::string& path, const std::string& host, std::uint16_t port,
                                 const HostKey& key) {
    std::string error;
    const auto contents = platform::readFile(path, &error);
    std::error_code ec;
    if (!contents && std::filesystem::exists(platform::pathFromUtf8(path), ec)) {
        return core::Status::error(error); // never risk rewriting a file we cannot read
    }
    // Rewrite the file a symlink points to, not the link itself.
    const std::string target = contents ? platform::pathToUtf8(std::filesystem::canonical(platform::pathFromUtf8(path), ec))
                                        : path;
    std::string kept;
    if (contents) {
        std::istringstream lines(*contents);
        std::string line;
        while (std::getline(lines, line)) {
            std::string trimmed = line;
            if (!trimmed.empty() && trimmed.back() == '\r') {
                trimmed.pop_back();
            }
            bool drop = false;
            if (!trimmed.empty() && trimmed[0] != '#') {
                // Parse this single line on its own and see whether it names the host with another key.
                DetachedSession scratch;
                if (LIBSSH2_KNOWNHOSTS* single = scratch.session ? libssh2_knownhost_init(scratch.session) : nullptr) {
                    if (libssh2_knownhost_readline(single, trimmed.c_str(), trimmed.size(),
                                                   LIBSSH2_KNOWNHOST_FILE_OPENSSH) == 0) {
                        drop = checkIn(single, host, port, key) == HostKeyCheck::Mismatch;
                    }
                    libssh2_knownhost_free(single);
                }
            }
            if (!drop) {
                kept += line + "\n";
            }
        }
    }
    if (auto status = platform::writeFileAtomic(ec ? path : target, kept, true); !status) {
        return status;
    }
    // Reload so the in-memory set no longer contains the replaced key.
    if (m->hosts) {
        libssh2_knownhost_free(m->hosts);
        m->hosts = libssh2_knownhost_init(m->owner.session);
        load(path);
    }
    return append(path, host, port, key);
}

std::string expandHome(const std::string& path) {
    if (path == "~" || path.rfind("~/", 0) == 0) {
        return platform::homeDirectory() + path.substr(1);
    }
    return path;
}

bool isPrivateKeyEncrypted(const std::string& contents) {
    if (contents.find("ENCRYPTED") != std::string::npos) {
        return true; // PEM: "Proc-Type: 4,ENCRYPTED" or "BEGIN ENCRYPTED PRIVATE KEY"
    }
    static constexpr std::string_view kBegin = "-----BEGIN OPENSSH PRIVATE KEY-----";
    static constexpr std::string_view kEnd = "-----END OPENSSH PRIVATE KEY-----";
    const auto begin = contents.find(kBegin);
    const auto end = contents.find(kEnd);
    if (begin == std::string::npos || end == std::string::npos || end < begin) {
        return false;
    }
    const auto decoded =
        core::base64Decode(std::string_view(contents).substr(begin + kBegin.size(), end - begin - kBegin.size()));
    // "openssh-key-v1\0" then a length-prefixed cipher name ("none" when unencrypted).
    static constexpr std::string_view kMagic{"openssh-key-v1\0", 15};
    if (!decoded || decoded->size() < kMagic.size() + 4 || decoded->compare(0, kMagic.size(), kMagic) != 0) {
        return false;
    }
    const auto* p = reinterpret_cast<const unsigned char*>(decoded->data()) + kMagic.size();
    const std::size_t length = (std::size_t{p[0]} << 24) | (std::size_t{p[1]} << 16) | (std::size_t{p[2]} << 8) | p[3];
    if (kMagic.size() + 4 + length > decoded->size()) {
        return false;
    }
    return decoded->substr(kMagic.size() + 4, length) != "none";
}

} // namespace wt::ssh
