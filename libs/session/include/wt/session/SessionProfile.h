#pragma once

#include "wt/core/Json.h"
#include "wt/core/Status.h"

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace wt::session {

enum class SessionKind { Local, Ssh };
enum class AuthMethod { Auto, Password, PublicKey, Agent, KeyboardInteractive };
enum class ProxyKind { None, Socks5, HttpConnect, JumpHost };

struct ProxySettings {
    ProxyKind kind = ProxyKind::None;
    std::string host; // Socks5 / HttpConnect
    std::uint16_t port = 0;
    std::string username;
    std::string credentialId;  // vault entry with the proxy password
    std::string jumpProfileId; // JumpHost: SSH profile used as the bastion

    friend bool operator==(const ProxySettings&, const ProxySettings&) = default;
};

struct ReconnectSettings {
    bool enabled = true;
    int maxAttempts = 5;
    int initialDelayMs = 1000;
    int maxDelayMs = 30000;

    friend bool operator==(const ReconnectSettings&, const ReconnectSettings&) = default;
};

struct LocalSettings {
    std::string program; // empty: login shell
    std::vector<std::string> arguments;
    std::string workingDirectory;
    bool loginShell = false;
    std::vector<std::pair<std::string, std::string>> environment;

    friend bool operator==(const LocalSettings&, const LocalSettings&) = default;
};

struct SshSettings {
    std::string host;
    std::uint16_t port = 22;
    std::string username;
    AuthMethod auth = AuthMethod::Auto;
    std::string identityFile;  // private key for PublicKey (and Auto)
    std::string credentialId;  // vault entry holding the password or key passphrase
    int connectTimeoutSeconds = 15;
    int keepAliveSeconds = 30; // 0 disables keep-alives
    std::string remoteCommand; // empty: interactive shell
    std::vector<std::pair<std::string, std::string>> environment;
    ProxySettings proxy;
    ReconnectSettings reconnect;

    friend bool operator==(const SshSettings&, const SshSettings&) = default;
};

struct SessionProfile {
    std::string id;
    std::string name;
    std::string group; // "/"-separated folder path, empty for the top level
    SessionKind kind = SessionKind::Local;
    LocalSettings local;
    SshSettings ssh;
    std::string theme; // empty: application theme
    std::string notes;

    // "user@host:port" for SSH, the program for local sessions.
    std::string target() const;

    core::Json toJson() const;
    static std::optional<SessionProfile> fromJson(const core::Json& json, std::string* error = nullptr);

    friend bool operator==(const SessionProfile&, const SessionProfile&) = default;
};

// Collection of profiles persisted as sessions.json. Secrets never live here: profiles refer to
// vault entries by credential id.
class SessionStore {
public:
    static constexpr int kVersion = 1;

    const std::vector<SessionProfile>& profiles() const { return m_profiles; }
    const SessionProfile* find(std::string_view id) const;
    std::vector<std::string> groups() const;

    // Inserts or replaces (by id). Assigns a fresh id when `profile.id` is empty; returns the id.
    std::string upsert(SessionProfile profile);
    bool remove(std::string_view id);
    // Copy with a new id and "<name> (copy)".
    std::optional<std::string> duplicate(std::string_view id);

    // Problems that would prevent connecting (missing host, bad port, jump cycles...).
    std::vector<std::string> validate(const SessionProfile& profile) const;
    // The chain of jump-host profiles for `profile`, nearest first; error on cycles/missing hosts.
    core::Status jumpChain(const SessionProfile& profile, std::vector<const SessionProfile*>& chain) const;

    core::Json toJson() const;
    static SessionStore fromJson(const core::Json& json, std::vector<std::string>* warnings = nullptr);

    core::Status load(std::string_view path); // a missing file yields an empty store
    core::Status save(std::string_view path) const;

    static std::string generateId();

private:
    std::vector<SessionProfile> m_profiles;
};

} // namespace wt::session
