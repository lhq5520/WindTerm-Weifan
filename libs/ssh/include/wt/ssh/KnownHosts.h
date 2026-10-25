#pragma once

#include "wt/core/Status.h"
#include "wt/ssh/SshTypes.h"

#include <cstdint>
#include <memory>
#include <string>

namespace wt::ssh {

// OpenSSH known_hosts handling on top of libssh2_knownhost (plain and hashed entries,
// "[host]:port" for non-standard ports).
class KnownHosts {
public:
    KnownHosts();
    ~KnownHosts();
    KnownHosts(const KnownHosts&) = delete;
    KnownHosts& operator=(const KnownHosts&) = delete;

    // Reads a known_hosts file; a missing file is not an error. Lines libssh2 cannot parse
    // (unsupported key types, @cert-authority) are skipped but never removed from the file.
    core::Status load(const std::string& path);
    HostKeyCheck check(const std::string& host, std::uint16_t port, const HostKey& key) const;

    // Appends an entry for host:port to `path` (and to this in-memory set).
    core::Status append(const std::string& path, const std::string& host, std::uint16_t port, const HostKey& key);
    // Replaces every entry of host:port that has a different key of the same type, then appends.
    core::Status replace(const std::string& path, const std::string& host, std::uint16_t port, const HostKey& key);

    // One known_hosts line for host:port (without a trailing newline).
    static std::string formatLine(const std::string& host, std::uint16_t port, const HostKey& key);

private:
    struct Impl;
    std::unique_ptr<Impl> m;
};

// Expands a leading "~" to the home directory.
std::string expandHome(const std::string& path);

// True if a private key file's contents are passphrase-protected (OpenSSH or PEM format).
bool isPrivateKeyEncrypted(const std::string& contents);

} // namespace wt::ssh
