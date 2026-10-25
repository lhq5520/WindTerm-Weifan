#include "wt/session/SessionProfile.h"

#include "wt/platform/Files.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <random>
#include <set>

namespace wt::session {
namespace {

template <typename Enum>
struct EnumName {
    Enum value;
    const char* name;
};

constexpr EnumName<SessionKind> kKinds[] = {{SessionKind::Local, "local"}, {SessionKind::Ssh, "ssh"}};
constexpr EnumName<AuthMethod> kAuthMethods[] = {{AuthMethod::Auto, "auto"},
                                                 {AuthMethod::Password, "password"},
                                                 {AuthMethod::PublicKey, "publickey"},
                                                 {AuthMethod::Agent, "agent"},
                                                 {AuthMethod::KeyboardInteractive, "keyboard-interactive"}};
constexpr EnumName<ProxyKind> kProxyKinds[] = {{ProxyKind::None, "none"},
                                               {ProxyKind::Socks5, "socks5"},
                                               {ProxyKind::HttpConnect, "http"},
                                               {ProxyKind::JumpHost, "jump"}};

template <typename Enum, std::size_t N>
const char* toName(const EnumName<Enum> (&table)[N], Enum value) {
    for (const auto& entry : table) {
        if (entry.value == value) {
            return entry.name;
        }
    }
    return table[0].name;
}

template <typename Enum, std::size_t N>
std::optional<Enum> fromName(const EnumName<Enum> (&table)[N], const core::Json& json, Enum fallback) {
    if (json.isNull()) {
        return fallback;
    }
    for (const auto& entry : table) {
        if (json.toString() == entry.name) {
            return entry.value;
        }
    }
    return std::nullopt;
}

core::Json stringList(const std::vector<std::string>& values) {
    core::Json list = core::Json::array();
    for (const auto& value : values) {
        list.push(value);
    }
    return list;
}

core::Json envToJson(const std::vector<std::pair<std::string, std::string>>& env) {
    core::Json list = core::Json::array();
    for (const auto& [name, value] : env) {
        core::Json item;
        item["name"] = name;
        item["value"] = value;
        list.push(std::move(item));
    }
    return list;
}

std::vector<std::pair<std::string, std::string>> envFromJson(const core::Json& json) {
    std::vector<std::pair<std::string, std::string>> env;
    for (const auto& item : json.items()) {
        const std::string name = item["name"].toString();
        if (!name.empty()) {
            env.emplace_back(name, item["value"].toString());
        }
    }
    return env;
}

std::uint16_t portFrom(const core::Json& json, std::uint16_t fallback) {
    const auto value = json.toInt(fallback);
    return value >= 0 && value <= 65535 ? static_cast<std::uint16_t>(value) : fallback;
}

int intFrom(const core::Json& json, int fallback, int low, int high) {
    return static_cast<int>(std::clamp<std::int64_t>(json.toInt(fallback), low, high));
}

} // namespace

std::string SessionProfile::target() const {
    if (kind == SessionKind::Local) {
        return local.program.empty() ? std::string("(login shell)") : local.program;
    }
    std::string text = ssh.username.empty() ? ssh.host : ssh.username + "@" + ssh.host;
    if (ssh.port != 22) {
        text += ":" + std::to_string(ssh.port);
    }
    return text;
}

core::Json SessionProfile::toJson() const {
    core::Json json;
    json["id"] = id;
    json["name"] = name;
    json["group"] = group;
    json["kind"] = toName(kKinds, kind);
    json["theme"] = theme;
    json["notes"] = notes;
    if (kind == SessionKind::Local) {
        core::Json& l = json["local"];
        l["program"] = local.program;
        l["arguments"] = stringList(local.arguments);
        l["workingDirectory"] = local.workingDirectory;
        l["loginShell"] = local.loginShell;
        l["environment"] = envToJson(local.environment);
    } else {
        core::Json& s = json["ssh"];
        s["host"] = ssh.host;
        s["port"] = ssh.port;
        s["username"] = ssh.username;
        s["auth"] = toName(kAuthMethods, ssh.auth);
        s["identityFile"] = ssh.identityFile;
        s["credentialId"] = ssh.credentialId;
        s["connectTimeoutSeconds"] = ssh.connectTimeoutSeconds;
        s["keepAliveSeconds"] = ssh.keepAliveSeconds;
        s["remoteCommand"] = ssh.remoteCommand;
        s["environment"] = envToJson(ssh.environment);
        core::Json& p = s["proxy"];
        p["kind"] = toName(kProxyKinds, ssh.proxy.kind);
        p["host"] = ssh.proxy.host;
        p["port"] = ssh.proxy.port;
        p["username"] = ssh.proxy.username;
        p["credentialId"] = ssh.proxy.credentialId;
        p["jumpProfileId"] = ssh.proxy.jumpProfileId;
        core::Json& r = s["reconnect"];
        r["enabled"] = ssh.reconnect.enabled;
        r["maxAttempts"] = ssh.reconnect.maxAttempts;
        r["initialDelayMs"] = ssh.reconnect.initialDelayMs;
        r["maxDelayMs"] = ssh.reconnect.maxDelayMs;
    }
    return json;
}

std::optional<SessionProfile> SessionProfile::fromJson(const core::Json& json, std::string* error) {
    auto fail = [error](std::string message) -> std::optional<SessionProfile> {
        if (error) {
            *error = std::move(message);
        }
        return std::nullopt;
    };
    SessionProfile p;
    p.id = json["id"].toString();
    p.name = json["name"].toString();
    if (p.id.empty()) {
        return fail("session without an id");
    }
    p.group = json["group"].toString();
    p.theme = json["theme"].toString();
    p.notes = json["notes"].toString();
    const auto kind = fromName(kKinds, json["kind"], SessionKind::Local);
    if (!kind) {
        return fail("session \"" + p.name + "\": unknown kind");
    }
    p.kind = *kind;
    const core::Json& l = json["local"];
    p.local.program = l["program"].toString();
    for (const auto& arg : l["arguments"].items()) {
        p.local.arguments.push_back(arg.toString());
    }
    p.local.workingDirectory = l["workingDirectory"].toString();
    p.local.loginShell = l["loginShell"].toBool(false);
    p.local.environment = envFromJson(l["environment"]);

    const core::Json& s = json["ssh"];
    p.ssh.host = s["host"].toString();
    p.ssh.port = portFrom(s["port"], 22);
    p.ssh.username = s["username"].toString();
    const auto auth = fromName(kAuthMethods, s["auth"], AuthMethod::Auto);
    if (!auth) {
        return fail("session \"" + p.name + "\": unknown authentication method");
    }
    p.ssh.auth = *auth;
    p.ssh.identityFile = s["identityFile"].toString();
    p.ssh.credentialId = s["credentialId"].toString();
    p.ssh.connectTimeoutSeconds = intFrom(s["connectTimeoutSeconds"], 15, 1, 600);
    p.ssh.keepAliveSeconds = intFrom(s["keepAliveSeconds"], 30, 0, 3600);
    p.ssh.remoteCommand = s["remoteCommand"].toString();
    p.ssh.environment = envFromJson(s["environment"]);
    const core::Json& proxy = s["proxy"];
    const auto proxyKind = fromName(kProxyKinds, proxy["kind"], ProxyKind::None);
    if (!proxyKind) {
        return fail("session \"" + p.name + "\": unknown proxy kind");
    }
    p.ssh.proxy.kind = *proxyKind;
    p.ssh.proxy.host = proxy["host"].toString();
    p.ssh.proxy.port = portFrom(proxy["port"], 0);
    p.ssh.proxy.username = proxy["username"].toString();
    p.ssh.proxy.credentialId = proxy["credentialId"].toString();
    p.ssh.proxy.jumpProfileId = proxy["jumpProfileId"].toString();
    const core::Json& r = s["reconnect"];
    p.ssh.reconnect.enabled = r["enabled"].toBool(true);
    p.ssh.reconnect.maxAttempts = intFrom(r["maxAttempts"], 5, 0, 1000);
    p.ssh.reconnect.initialDelayMs = intFrom(r["initialDelayMs"], 1000, 100, 600000);
    p.ssh.reconnect.maxDelayMs = intFrom(r["maxDelayMs"], 30000, p.ssh.reconnect.initialDelayMs, 3600000);
    return p;
}

const SessionProfile* SessionStore::find(std::string_view id) const {
    const auto it = std::find_if(m_profiles.begin(), m_profiles.end(), [id](const auto& p) { return p.id == id; });
    return it == m_profiles.end() ? nullptr : &*it;
}

std::vector<std::string> SessionStore::groups() const {
    std::set<std::string> unique;
    for (const auto& profile : m_profiles) {
        if (!profile.group.empty()) {
            unique.insert(profile.group);
        }
    }
    return {unique.begin(), unique.end()};
}

std::string SessionStore::upsert(SessionProfile profile) {
    if (profile.id.empty()) {
        profile.id = generateId();
    }
    for (auto& existing : m_profiles) {
        if (existing.id == profile.id) {
            existing = std::move(profile);
            return existing.id;
        }
    }
    m_profiles.push_back(std::move(profile));
    return m_profiles.back().id;
}

bool SessionStore::remove(std::string_view id) {
    const auto it = std::find_if(m_profiles.begin(), m_profiles.end(), [id](const auto& p) { return p.id == id; });
    if (it == m_profiles.end()) {
        return false;
    }
    m_profiles.erase(it);
    return true;
}

std::optional<std::string> SessionStore::duplicate(std::string_view id) {
    const SessionProfile* original = find(id);
    if (!original) {
        return std::nullopt;
    }
    SessionProfile copy = *original;
    copy.id.clear();
    copy.name += " (copy)";
    return upsert(std::move(copy));
}

core::Status SessionStore::jumpChain(const SessionProfile& profile, std::vector<const SessionProfile*>& chain) const {
    chain.clear();
    std::set<std::string> seen{profile.id};
    const SessionProfile* current = &profile;
    while (current->kind == SessionKind::Ssh && current->ssh.proxy.kind == ProxyKind::JumpHost) {
        const std::string& next = current->ssh.proxy.jumpProfileId;
        const SessionProfile* jump = find(next);
        if (!jump) {
            return core::Status::error("jump host session \"" + next + "\" does not exist");
        }
        if (jump->kind != SessionKind::Ssh) {
            return core::Status::error("jump host \"" + jump->name + "\" is not an SSH session");
        }
        if (!seen.insert(jump->id).second) {
            return core::Status::error("jump host chain of \"" + profile.name + "\" contains a cycle");
        }
        chain.push_back(jump);
        current = jump;
    }
    return core::Status::ok();
}

std::vector<std::string> SessionStore::validate(const SessionProfile& profile) const {
    std::vector<std::string> problems;
    if (profile.name.empty()) {
        problems.emplace_back("name is empty");
    }
    if (profile.kind == SessionKind::Ssh) {
        if (profile.ssh.host.empty()) {
            problems.emplace_back("host is empty");
        }
        if (profile.ssh.port == 0) {
            problems.emplace_back("port must be between 1 and 65535");
        }
        if (profile.ssh.auth == AuthMethod::PublicKey && profile.ssh.identityFile.empty()) {
            problems.emplace_back("public key authentication needs an identity file");
        }
        const ProxySettings& proxy = profile.ssh.proxy;
        if ((proxy.kind == ProxyKind::Socks5 || proxy.kind == ProxyKind::HttpConnect) &&
            (proxy.host.empty() || proxy.port == 0)) {
            problems.emplace_back("proxy host and port are required");
        }
        if (proxy.kind == ProxyKind::JumpHost) {
            std::vector<const SessionProfile*> chain;
            if (const auto status = jumpChain(profile, chain); !status) {
                problems.push_back(status.message());
            }
        }
    }
    return problems;
}

core::Json SessionStore::toJson() const {
    core::Json json;
    json["version"] = kVersion;
    core::Json list = core::Json::array();
    for (const auto& profile : m_profiles) {
        list.push(profile.toJson());
    }
    json["sessions"] = std::move(list);
    return json;
}

SessionStore SessionStore::fromJson(const core::Json& json, std::vector<std::string>* warnings) {
    SessionStore store;
    for (const auto& item : json["sessions"].items()) {
        std::string error;
        auto profile = SessionProfile::fromJson(item, &error);
        if (!profile) {
            if (warnings) {
                warnings->push_back(error);
            }
            continue;
        }
        if (store.find(profile->id)) {
            if (warnings) {
                warnings->push_back("duplicate session id \"" + profile->id + "\"");
            }
            continue;
        }
        store.m_profiles.push_back(std::move(*profile));
    }
    return store;
}

core::Status SessionStore::load(std::string_view path) {
    std::error_code ec;
    if (!std::filesystem::exists(platform::pathFromUtf8(path), ec)) {
        m_profiles.clear();
        return core::Status::ok();
    }
    std::string error;
    const auto text = platform::readFile(path, &error);
    if (!text) {
        return core::Status::error(error);
    }
    const auto json = core::Json::parse(*text, &error);
    if (!json) {
        return core::Status::error(std::string(path) + ": " + error);
    }
    *this = fromJson(*json);
    return core::Status::ok();
}

core::Status SessionStore::save(std::string_view path) const {
    return platform::writeFileAtomic(path, toJson().dump(2) + "\n", true);
}

std::string SessionStore::generateId() {
    static std::mt19937_64 engine(static_cast<std::uint64_t>(std::random_device{}()) ^
                                  static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()));
    char buffer[40];
    std::snprintf(buffer, sizeof(buffer), "%016llx%08llx", static_cast<unsigned long long>(engine()),
                  static_cast<unsigned long long>(engine() & 0xFFFFFFFFu));
    return buffer;
}

} // namespace wt::session
