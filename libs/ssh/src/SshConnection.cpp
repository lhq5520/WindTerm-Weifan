#include "wt/ssh/SshConnection.h"

#include "Libssh2Init.h"

#include "wt/core/Base64.h"
#include "wt/platform/Files.h"
#include "wt/ssh/KnownHosts.h"
#include "wt/ssh/Sftp.h"

#include <libssh2_sftp.h>

#include <cstdlib>
#include <cstring>
#include <thread>

#ifndef _WIN32
#include <poll.h>
#endif

namespace wt::ssh {
namespace {

constexpr int kMaxSecretAttempts = 3;

std::string hostKeyTypeName(int type) {
    switch (type) {
    case LIBSSH2_HOSTKEY_TYPE_RSA: return "ssh-rsa";
    case LIBSSH2_HOSTKEY_TYPE_DSS: return "ssh-dss";
    case LIBSSH2_HOSTKEY_TYPE_ECDSA_256: return "ecdsa-sha2-nistp256";
    case LIBSSH2_HOSTKEY_TYPE_ECDSA_384: return "ecdsa-sha2-nistp384";
    case LIBSSH2_HOSTKEY_TYPE_ECDSA_521: return "ecdsa-sha2-nistp521";
    case LIBSSH2_HOSTKEY_TYPE_ED25519: return "ssh-ed25519";
    default: return "unknown";
    }
}

bool contains(const char* list, const char* method) {
    if (!list) {
        return false;
    }
    const std::string_view methods(list);
    for (std::size_t pos = 0; pos <= methods.size();) {
        const auto comma = methods.find(',', pos);
        const auto item = methods.substr(pos, comma == std::string_view::npos ? std::string_view::npos : comma - pos);
        if (item == method) {
            return true;
        }
        if (comma == std::string_view::npos) {
            break;
        }
        pos = comma + 1;
    }
    return false;
}

std::vector<std::string> defaultIdentityFiles() {
    std::vector<std::string> files;
    for (const char* name : {"~/.ssh/id_ed25519", "~/.ssh/id_ecdsa", "~/.ssh/id_rsa"}) {
        const std::string path = expandHome(name);
        std::error_code ec;
        if (std::filesystem::exists(platform::pathFromUtf8(path), ec)) {
            files.push_back(path);
        }
    }
    return files;
}

void wipe(std::string& secret) {
    volatile char* p = secret.data();
    for (std::size_t i = 0; i < secret.size(); ++i) {
        p[i] = 0;
    }
    secret.clear();
}

} // namespace

std::string libssh2Error(LIBSSH2_SESSION* session) {
    if (!session) {
        return "no SSH session";
    }
    char* message = nullptr;
    int length = 0;
    const int code = libssh2_session_last_error(session, &message, &length, 0);
    if (!message || length == 0) {
        return "error " + std::to_string(code);
    }
    return std::string(message, static_cast<std::size_t>(length));
}

// ---------------------------------------------------------------------------------------------
// Aborter

void Aborter::abort() {
    std::lock_guard lock(m_mutex);
    m_aborted = true;
    net::shutdownSocket(m_socket);
}

void Aborter::attach(net::NativeSocket socket) {
    std::lock_guard lock(m_mutex);
    m_socket = socket;
    if (m_aborted) {
        net::shutdownSocket(socket);
    }
}

void Aborter::detach() {
    std::lock_guard lock(m_mutex);
    m_socket = net::kInvalidSocket;
}

// ---------------------------------------------------------------------------------------------
// Jump host bridge: relays between a direct-tcpip channel on the jump connection and one end of
// a socket pair; the inner connection runs its SSH session over the other end.

struct SshConnection::Bridge {
    std::shared_ptr<SshConnection> jump;
    LIBSSH2_CHANNEL* channel = nullptr;
    net::Socket local;
    std::thread thread;
    std::atomic<bool> stop{false};

    ~Bridge() {
        stop = true;
        local.shutdown();
        if (thread.joinable()) {
            thread.join();
        }
        if (channel) {
            libssh2_session_set_blocking(jump->session(), 1);
            libssh2_session_set_timeout(jump->session(), 1000);
            libssh2_channel_free(channel);
        }
    }

    void run() {
        LIBSSH2_SESSION* session = jump->session();
        libssh2_session_set_blocking(session, 0);
        std::string toChannel;
        char buffer[32768];
        bool localOpen = true;
        bool eofPending = false;
        while (!stop) {
            // Socket -> channel.
            if (localOpen && toChannel.empty() && net::waitReadable(local.native(), std::chrono::milliseconds(0))) {
                std::size_t n = 0;
                if (!local.receiveSome(buffer, sizeof(buffer), n) || n == 0) {
                    localOpen = false;
                    eofPending = true;
                } else {
                    toChannel.assign(buffer, n);
                }
            }
            if (eofPending && toChannel.empty() && libssh2_channel_send_eof(channel) != LIBSSH2_ERROR_EAGAIN) {
                eofPending = false;
            }
            while (!toChannel.empty()) {
                const auto n = libssh2_channel_write(channel, toChannel.data(), toChannel.size());
                if (n == LIBSSH2_ERROR_EAGAIN) {
                    break;
                }
                if (n < 0) {
                    local.shutdown();
                    return;
                }
                toChannel.erase(0, static_cast<std::size_t>(n));
            }
            // Channel -> socket.
            for (;;) {
                const auto n = libssh2_channel_read(channel, buffer, sizeof(buffer));
                if (n == LIBSSH2_ERROR_EAGAIN) {
                    break;
                }
                if (n <= 0) {
                    if (n < 0 || libssh2_channel_eof(channel)) {
                        local.shutdown();
                        return;
                    }
                    break;
                }
                if (!local.sendAll({buffer, static_cast<std::size_t>(n)})) {
                    return;
                }
            }
            if (libssh2_channel_eof(channel)) {
                local.shutdown();
                return;
            }
            int nextKeepAlive = 0;
            libssh2_keepalive_send(session, &nextKeepAlive);
            // Wait for either side.
#ifdef _WIN32
            WSAPOLLFD fds[2] = {{static_cast<SOCKET>(jump->socket()), POLLRDNORM, 0},
                                {static_cast<SOCKET>(local.native()), POLLRDNORM, 0}};
            if (!toChannel.empty() || (libssh2_session_block_directions(session) & LIBSSH2_SESSION_BLOCK_OUTBOUND)) {
                fds[0].events |= POLLWRNORM;
            }
            WSAPoll(fds, localOpen && toChannel.empty() ? 2 : 1, 50);
#else
            pollfd fds[2] = {{jump->socket(), POLLIN, 0}, {local.native(), POLLIN, 0}};
            if (!toChannel.empty() || (libssh2_session_block_directions(session) & LIBSSH2_SESSION_BLOCK_OUTBOUND)) {
                fds[0].events |= POLLOUT;
            }
            ::poll(fds, localOpen && toChannel.empty() ? 2 : 1, 50);
#endif
        }
    }
};

// ---------------------------------------------------------------------------------------------
// Keyboard-interactive

struct KbdIntContext {
    static void respond(const char* name, int nameLength, const char* instruction, int instructionLength,
                        int promptCount, const LIBSSH2_USERAUTH_KBDINT_PROMPT* prompts,
                        LIBSSH2_USERAUTH_KBDINT_RESPONSE* responses, void** abstract) {
        auto* self = static_cast<SshConnection*>(*abstract);
        std::vector<KeyboardPrompt> list;
        for (int i = 0; i < promptCount; ++i) {
            list.push_back({std::string(reinterpret_cast<const char*>(prompts[i].text), prompts[i].length),
                            prompts[i].echo != 0});
        }
        std::optional<std::vector<std::string>> answers;
        // A single hidden prompt is a password prompt: use the known password once.
        if (promptCount == 1 && !list[0].echo && self->m_options.password && self->m_kbdRound == 0) {
            answers = std::vector<std::string>{*self->m_options.password};
            self->m_usedPassword = self->m_options.password;
        } else if (promptCount > 0 && self->m_ui) {
            answers = self->m_ui->keyboardInteractive(std::string(name ? name : "", static_cast<std::size_t>(nameLength)),
                                                      std::string(instruction ? instruction : "",
                                                                  static_cast<std::size_t>(instructionLength)),
                                                      list);
            if (!answers) {
                self->m_kbdCancelled = true;
            } else if (promptCount == 1 && !list[0].echo && !answers->empty()) {
                self->m_usedPassword = answers->front();
            }
        }
        ++self->m_kbdRound;
        for (int i = 0; i < promptCount; ++i) {
            const std::string text = answers && i < static_cast<int>(answers->size()) ? (*answers)[static_cast<std::size_t>(i)] : "";
            responses[i].text = static_cast<char*>(std::malloc(text.size() + 1));
            if (responses[i].text) {
                std::memcpy(responses[i].text, text.c_str(), text.size() + 1);
                responses[i].length = static_cast<unsigned int>(text.size());
            }
        }
    }
};

// ---------------------------------------------------------------------------------------------
// SshConnection

std::shared_ptr<SshConnection> SshConnection::connect(const ConnectOptions& options, SshInteraction& ui,
                                                      std::string* error, std::shared_ptr<SshConnection> via,
                                                      Aborter* aborter) {
    ensureLibssh2();
    auto fail = [error, aborter](std::string message) -> std::shared_ptr<SshConnection> {
        if (aborter) {
            aborter->detach(); // the socket is about to be closed (and its number reused)
        }
        if (error) {
            *error = std::move(message);
        }
        return nullptr;
    };
    std::shared_ptr<SshConnection> self(new SshConnection());
    self->m_options = options;
    const std::string target = options.host + ":" + std::to_string(options.port);

    if (via) {
        ui.info("Opening a tunnel to " + target + " through " + via->options().host + "...");
        std::string channelError;
        LIBSSH2_CHANNEL* channel = via->openDirectTcpip(options.host, options.port, &channelError);
        if (!channel) {
            return fail("jump host " + via->options().host + " cannot reach " + target + ": " + channelError);
        }
        auto bridge = std::make_unique<Bridge>();
        bridge->jump = via;
        bridge->channel = channel;
        if (auto status = net::socketPair(self->m_socket, bridge->local); !status) {
            return fail(status.message());
        }
        Bridge* raw = bridge.get();
        bridge->thread = std::thread([raw] { raw->run(); });
        self->m_bridge = std::move(bridge);
        self->m_via = std::move(via);
    } else {
        ui.info("Connecting to " + target + "...");
        net::Socket socket;
        auto attach = [aborter](net::NativeSocket fd) {
            if (aborter) {
                aborter->attach(fd); // abortable during proxy negotiation too
            }
        };
        if (auto status = net::connectThrough(options.proxy, options.host, options.port, options.timeout, socket,
                                              aborter ? aborter->flag() : nullptr, attach);
            !status) {
            return fail(status.message());
        }
        socket.setKeepAlive(true);
        if (options.keepAliveSeconds > 0) {
            // Unanswered keep-alives (dead peer, no RST) drop the connection after ~3 intervals.
            socket.setUserTimeout(options.keepAliveSeconds * 3 + 5);
        }
        self->m_socket = std::move(socket);
    }
    if (aborter) {
        aborter->attach(self->m_socket.native());
        if (aborter->aborted()) {
            return fail("cancelled");
        }
    }

    self->m_session = libssh2_session_init_ex(nullptr, nullptr, nullptr, self.get());
    if (!self->m_session) {
        return fail("cannot create an SSH session");
    }
    libssh2_session_set_blocking(self->m_session, 1);
    libssh2_session_set_timeout(self->m_session, static_cast<long>(options.timeout.count()));
    libssh2_session_flag(self->m_session, LIBSSH2_FLAG_COMPRESS, 0);
    if (libssh2_session_handshake(self->m_session, static_cast<libssh2_socket_t>(self->m_socket.native())) != 0) {
        return fail(aborter && aborter->aborted() ? "cancelled"
                                                  : "SSH handshake with " + target + " failed: " + libssh2Error(self->m_session));
    }
    if (auto status = self->verifyHostKey(ui); !status) {
        return fail(status.message());
    }
    if (auto status = self->authenticate(ui); !status) {
        return fail(aborter && aborter->aborted() ? "cancelled" : status.message());
    }
    if (options.keepAliveSeconds > 0) {
        libssh2_keepalive_config(self->m_session, 1, static_cast<unsigned>(options.keepAliveSeconds));
    }
    // Blocking calls after connecting must not time out on idle sessions.
    libssh2_session_set_timeout(self->m_session, 0);
    if (aborter) {
        aborter->detach(); // the aborter covers connecting only; the socket now belongs to us
    }
    return self;
}

SshConnection::~SshConnection() {
    if (m_session) {
        libssh2_session_set_blocking(m_session, 1);
        libssh2_session_set_timeout(m_session, 2000);
        libssh2_session_disconnect(m_session, "closed by the user");
        libssh2_session_free(m_session);
    }
    m_bridge.reset(); // before the jump connection goes
    m_socket.close();
    for (auto* secret : {&m_options.password, &m_options.passphrase, &m_usedPassword, &m_usedPassphrase}) {
        if (*secret) {
            wipe(**secret);
        }
    }
}

std::string SshConnection::serverBanner() const {
    const char* banner = m_session ? libssh2_session_banner_get(m_session) : nullptr;
    return banner ? banner : "";
}

std::string SshConnection::lastError() const {
    return libssh2Error(m_session);
}

void SshConnection::setBlocking(bool blocking) {
    libssh2_session_set_blocking(m_session, blocking ? 1 : 0);
}

bool SshConnection::isAlive() const {
    return m_session && m_socket.isValid();
}

core::Status SshConnection::verifyHostKey(SshInteraction& ui) {
    std::size_t length = 0;
    int type = 0;
    const char* key = libssh2_session_hostkey(m_session, &length, &type);
    if (!key || length == 0) {
        return core::Status::error("the server sent no host key");
    }
    m_hostKey.blob.assign(key, length);
    m_hostKey.libssh2Type = type;
    m_hostKey.type = hostKeyTypeName(type);
    if (const char* hash = libssh2_hostkey_hash(m_session, LIBSSH2_HOSTKEY_HASH_SHA256)) {
        std::string fingerprint = core::base64Encode(std::string_view(hash, 32));
        while (!fingerprint.empty() && fingerprint.back() == '=') {
            fingerprint.pop_back();
        }
        m_hostKey.fingerprint = "SHA256:" + fingerprint;
    }

    KnownHosts knownHosts;
    const std::string path = expandHome(m_options.knownHostsPath);
    if (!path.empty()) {
        if (auto status = knownHosts.load(path); !status) {
            ui.info("warning: " + status.message());
        }
    }
    const HostKeyCheck check = path.empty() ? HostKeyCheck::NotFound
                                            : knownHosts.check(m_options.host, m_options.port, m_hostKey);
    if (check == HostKeyCheck::Match) {
        return core::Status::ok();
    }
    const HostKeyDecision decision = ui.confirmHostKey(m_options.host, m_options.port, m_hostKey, check);
    if (decision == HostKeyDecision::Reject) {
        return core::Status::error(check == HostKeyCheck::Mismatch
                                       ? "Host key verification failed: the key of " + m_options.host +
                                             " has changed (" + m_hostKey.fingerprint + ")"
                                       : "Host key of " + m_options.host + " was not accepted");
    }
    if (decision == HostKeyDecision::AcceptAndSave && !path.empty()) {
        const core::Status status = check == HostKeyCheck::Mismatch
                                        ? knownHosts.replace(path, m_options.host, m_options.port, m_hostKey)
                                        : knownHosts.append(path, m_options.host, m_options.port, m_hostKey);
        if (!status) {
            ui.info("warning: could not save the host key: " + status.message());
        }
    }
    return core::Status::ok();
}

bool SshConnection::tryAgent() {
    LIBSSH2_AGENT* agent = libssh2_agent_init(m_session);
    if (!agent) {
        return false;
    }
    bool ok = false;
    if (libssh2_agent_connect(agent) == 0 && libssh2_agent_list_identities(agent) == 0) {
        libssh2_agent_publickey* identity = nullptr;
        libssh2_agent_publickey* previous = nullptr;
        while (libssh2_agent_get_identity(agent, &identity, previous) == 0) {
            if (libssh2_agent_userauth(agent, m_options.username.c_str(), identity) == 0) {
                ok = true;
                break;
            }
            previous = identity;
        }
        libssh2_agent_disconnect(agent);
    }
    libssh2_agent_free(agent);
    if (ok) {
        m_authMethod = "agent";
    }
    return ok;
}

// 1 = authenticated, 0 = rejected, -1 = could not try (missing file).
int SshConnection::tryKeyFile(const std::string& file, SshInteraction& ui, bool& cancelled) {
    const std::string path = expandHome(file);
    std::string error;
    auto contents = platform::readFile(path, &error);
    if (!contents) {
        ui.info("cannot read " + path + ": " + error);
        return -1;
    }
    const bool encrypted = isPrivateKeyEncrypted(*contents);
    wipe(*contents); // libssh2 reads the file itself; do not keep a copy of the private key
    std::optional<std::string> passphrase = encrypted ? m_options.passphrase : std::optional<std::string>("");
    for (int attempt = 0; attempt < kMaxSecretAttempts; ++attempt) {
        if (!passphrase) {
            passphrase = ui.askPassphrase(path);
            if (!passphrase) {
                cancelled = true;
                return 0;
            }
        }
        const int rc = libssh2_userauth_publickey_fromfile_ex(m_session, m_options.username.c_str(),
                                                              static_cast<unsigned>(m_options.username.size()),
                                                              nullptr, path.c_str(), passphrase->c_str());
        if (rc == 0) {
            if (encrypted) {
                m_usedPassphrase = passphrase;
            }
            m_authMethod = "publickey";
            return 1;
        }
        // A wrong passphrase means the key file cannot be decoded (LIBSSH2_ERROR_FILE); any other
        // error means the server rejected the key.
        if (!encrypted || rc != LIBSSH2_ERROR_FILE) {
            m_lastAuthError = path + ": " + lastError();
            return 0;
        }
        ui.info("wrong passphrase for " + path);
        wipe(*passphrase);
        passphrase.reset();
    }
    return 0;
}

int SshConnection::tryPassword(SshInteraction& ui, bool& cancelled) {
    std::optional<std::string> password = m_options.password;
    const std::string prompt = m_options.username + "@" + m_options.host + "'s password:";
    for (int attempt = 0; attempt < kMaxSecretAttempts; ++attempt) {
        if (!password) {
            password = ui.askPassword(prompt);
            if (!password) {
                cancelled = true;
                return 0;
            }
        }
        const int rc = libssh2_userauth_password_ex(m_session, m_options.username.c_str(),
                                                    static_cast<unsigned>(m_options.username.size()), password->c_str(),
                                                    static_cast<unsigned>(password->size()), nullptr);
        if (rc == 0) {
            m_usedPassword = password;
            m_authMethod = "password";
            return 1;
        }
        if (rc == LIBSSH2_ERROR_PASSWORD_EXPIRED) {
            ui.info("the password has expired");
            return 0;
        }
        if (rc != LIBSSH2_ERROR_AUTHENTICATION_FAILED) {
            return 0; // e.g. method not allowed or the connection dropped
        }
        ui.info("Permission denied, please try again.");
        wipe(*password);
        password.reset();
    }
    return 0;
}

int SshConnection::tryKeyboardInteractive(SshInteraction& ui, bool& cancelled) {
    m_ui = &ui;
    for (int attempt = 0; attempt < kMaxSecretAttempts && !m_kbdCancelled; ++attempt) {
        const int rc = libssh2_userauth_keyboard_interactive_ex(
            m_session, m_options.username.c_str(), static_cast<unsigned>(m_options.username.size()),
            &KbdIntContext::respond);
        if (rc == 0) {
            m_ui = nullptr;
            m_authMethod = "keyboard-interactive";
            return 1;
        }
        m_usedPassword.reset();
        if (rc != LIBSSH2_ERROR_AUTHENTICATION_FAILED) {
            break;
        }
    }
    m_ui = nullptr;
    cancelled = m_kbdCancelled;
    return 0;
}

core::Status SshConnection::authenticate(SshInteraction& ui) {
    const std::string& user = m_options.username;
    if (user.empty()) {
        return core::Status::error("no user name given");
    }
    const char* methods = libssh2_userauth_list(m_session, user.c_str(), static_cast<unsigned>(user.size()));
    if (!methods) {
        if (libssh2_userauth_authenticated(m_session)) {
            m_authMethod = "none";
            return core::Status::ok();
        }
        return core::Status::error("cannot get authentication methods: " + lastError());
    }
    std::vector<std::string> tried;
    bool cancelled = false;
    const AuthMethod method = m_options.auth;
    auto allowed = [&](AuthMethod wanted) { return method == AuthMethod::Auto || method == wanted; };

    if (allowed(AuthMethod::Agent) && contains(methods, "publickey")) {
        tried.emplace_back("agent");
        if (tryAgent()) {
            return core::Status::ok();
        }
    }
    if (allowed(AuthMethod::PublicKey) && contains(methods, "publickey")) {
        std::vector<std::string> files;
        if (!m_options.identityFile.empty()) {
            files.push_back(m_options.identityFile);
        } else if (method == AuthMethod::Auto && m_options.tryDefaultKeys) {
            files = defaultIdentityFiles();
        }
        for (const std::string& file : files) {
            tried.push_back("publickey (" + file + ")");
            const int rc = tryKeyFile(file, ui, cancelled);
            if (rc == 1) {
                return core::Status::ok();
            }
            if (cancelled) {
                return core::Status::error("authentication cancelled");
            }
        }
    }
    if (allowed(AuthMethod::KeyboardInteractive) && contains(methods, "keyboard-interactive")) {
        tried.emplace_back("keyboard-interactive");
        if (tryKeyboardInteractive(ui, cancelled) == 1) {
            return core::Status::ok();
        }
        if (cancelled) {
            return core::Status::error("authentication cancelled");
        }
    }
    if (allowed(AuthMethod::Password) && contains(methods, "password")) {
        tried.emplace_back("password");
        if (tryPassword(ui, cancelled) == 1) {
            return core::Status::ok();
        }
        if (cancelled) {
            return core::Status::error("authentication cancelled");
        }
    }
    std::string list;
    for (const auto& item : tried) {
        list += (list.empty() ? "" : ", ") + item;
    }
    std::string message = "Authentication failed for " + user + "@" + m_options.host +
                          (list.empty() ? std::string(" (server offers: ") + methods + ")" : " (tried " + list + ")");
    if (!m_lastAuthError.empty()) {
        message += ": " + m_lastAuthError;
    }
    return core::Status::error(message);
}

LIBSSH2_CHANNEL* SshConnection::openShellChannel(const ShellOptions& options, std::string* error) {
    auto fail = [&](const std::string& what) -> LIBSSH2_CHANNEL* {
        if (error) {
            *error = what + ": " + lastError();
        }
        return nullptr;
    };
    LIBSSH2_CHANNEL* channel = libssh2_channel_open_session(m_session);
    if (!channel) {
        return fail("cannot open a session channel");
    }
    for (const auto& [name, value] : options.environment) {
        // Servers only accept variables listed in AcceptEnv; a refusal is not fatal.
        libssh2_channel_setenv_ex(channel, name.c_str(), static_cast<unsigned>(name.size()), value.c_str(),
                                  static_cast<unsigned>(value.size()));
    }
    if (libssh2_channel_request_pty_ex(channel, options.term.c_str(), static_cast<unsigned>(options.term.size()),
                                       nullptr, 0, options.columns, options.rows, options.pixelWidth,
                                       options.pixelHeight) != 0) {
        libssh2_channel_free(channel);
        return fail("the server refused a terminal");
    }
    const int rc = options.command.empty()
                       ? libssh2_channel_shell(channel)
                       : libssh2_channel_exec(channel, options.command.c_str());
    if (rc != 0) {
        libssh2_channel_free(channel);
        return fail(options.command.empty() ? "the server refused a shell" : "the server refused the command");
    }
    return channel;
}

core::Status SshConnection::execute(const std::string& command, std::string& output, int& exitStatus) {
    LIBSSH2_CHANNEL* channel = libssh2_channel_open_session(m_session);
    if (!channel) {
        return core::Status::error("cannot open a session channel: " + lastError());
    }
    if (libssh2_channel_exec(channel, command.c_str()) != 0) {
        libssh2_channel_free(channel);
        return core::Status::error("exec failed: " + lastError());
    }
    char buffer[8192];
    for (;;) {
        const auto n = libssh2_channel_read(channel, buffer, sizeof(buffer));
        if (n > 0) {
            output.append(buffer, static_cast<std::size_t>(n));
        } else if (n == 0) {
            break;
        } else {
            libssh2_channel_free(channel);
            return core::Status::error("read failed: " + lastError());
        }
    }
    libssh2_channel_close(channel);
    libssh2_channel_wait_closed(channel);
    exitStatus = libssh2_channel_get_exit_status(channel);
    libssh2_channel_free(channel);
    return core::Status::ok();
}

LIBSSH2_CHANNEL* SshConnection::openDirectTcpip(const std::string& host, std::uint16_t port, std::string* error) {
    LIBSSH2_CHANNEL* channel = libssh2_channel_direct_tcpip_ex(m_session, host.c_str(), port, "127.0.0.1", 0);
    if (!channel && error) {
        *error = lastError();
    }
    return channel;
}

std::unique_ptr<SftpSession> SshConnection::openSftp(std::string* error) {
    LIBSSH2_SFTP* sftp = libssh2_sftp_init(m_session);
    if (!sftp) {
        if (error) {
            *error = "cannot start SFTP: " + lastError();
        }
        return nullptr;
    }
    return std::unique_ptr<SftpSession>(new SftpSession(shared_from_this(), sftp));
}

} // namespace wt::ssh
