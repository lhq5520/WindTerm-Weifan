#include "SshBackend.h"

#include "AppContext.h"
#include "GuiInteraction.h"

#include <QCoreApplication>

#include <algorithm>

#include <cstdlib>

namespace {

wt::ssh::AuthMethod toSshAuth(wt::session::AuthMethod method) {
    switch (method) {
    case wt::session::AuthMethod::Password: return wt::ssh::AuthMethod::Password;
    case wt::session::AuthMethod::PublicKey: return wt::ssh::AuthMethod::PublicKey;
    case wt::session::AuthMethod::Agent: return wt::ssh::AuthMethod::Agent;
    case wt::session::AuthMethod::KeyboardInteractive: return wt::ssh::AuthMethod::KeyboardInteractive;
    case wt::session::AuthMethod::Auto: break;
    }
    return wt::ssh::AuthMethod::Auto;
}

std::string localUserName() {
    for (const char* name : {"USER", "LOGNAME", "USERNAME"}) {
        if (const char* value = std::getenv(name); value && *value) {
            return value;
        }
    }
    return "root";
}

bool hopFromProfile(const wt::session::SessionProfile& profile, AppContext* context, QWidget* parent,
                    wt::ssh::ConnectOptions& options, QString* error) {
    const auto& ssh = profile.ssh;
    options.host = ssh.host;
    options.port = ssh.port;
    options.username = ssh.username.empty() ? localUserName() : ssh.username;
    options.auth = toSshAuth(ssh.auth);
    options.identityFile = ssh.identityFile;
    options.certificateFile = ssh.certificateFile;
    options.knownHostsPath = context->knownHostsPath().toStdString();
    options.timeout = std::chrono::seconds(ssh.connectTimeoutSeconds);
    options.keepAliveSeconds = ssh.keepAliveSeconds;
    if (!ssh.credentialId.empty()) {
        if (auto secret = context->credential(ssh.credentialId, parent)) {
            // One stored secret serves as the password or as the key passphrase.
            options.password = *secret;
            options.passphrase = *secret;
        }
    }
    switch (ssh.proxy.kind) {
    case wt::session::ProxyKind::Socks5:
    case wt::session::ProxyKind::HttpConnect:
        options.proxy.type = ssh.proxy.kind == wt::session::ProxyKind::Socks5 ? wt::net::ProxyType::Socks5
                                                                               : wt::net::ProxyType::HttpConnect;
        options.proxy.host = ssh.proxy.host;
        options.proxy.port = ssh.proxy.port;
        options.proxy.username = ssh.proxy.username;
        if (!ssh.proxy.credentialId.empty()) {
            if (auto secret = context->credential(ssh.proxy.credentialId, parent)) {
                options.proxy.password = *secret;
            }
        }
        break;
    case wt::session::ProxyKind::None:
    case wt::session::ProxyKind::JumpHost: break;
    }
    if (options.host.empty()) {
        if (error) {
            *error = QObject::tr("Session \"%1\" has no host").arg(QString::fromStdString(profile.name));
        }
        return false;
    }
    return true;
}

wt::ssh::ForwardKind toSshKind(wt::session::ForwardKind kind) {
    switch (kind) {
    case wt::session::ForwardKind::Remote: return wt::ssh::ForwardKind::Remote;
    case wt::session::ForwardKind::Dynamic: return wt::ssh::ForwardKind::Dynamic;
    case wt::session::ForwardKind::Local: break;
    }
    return wt::ssh::ForwardKind::Local;
}

bool isPermanentFailure(const QString& error) {
    // Retrying cannot fix these; it would only repeat prompts.
    return error.contains(QLatin1String("Authentication")) || error.contains(QLatin1String("Host key")) ||
           error.contains(QLatin1String("cancelled")) || error.contains(QLatin1String("not accepted"));
}

} // namespace

wt::ssh::PortForward toSshForward(const wt::session::PortForwardSettings& forward) {
    wt::ssh::PortForward out;
    out.kind = toSshKind(forward.kind);
    out.bindAddress = forward.bindAddress;
    out.bindPort = forward.bindPort;
    out.targetHost = forward.targetHost;
    out.targetPort = forward.targetPort;
    return out;
}

// ---------------------------------------------------------------------------------------------
// SshPlan

bool SshPlan::fromProfile(const wt::session::SessionProfile& profile, AppContext* context, QWidget* dialogParent,
                          SshPlan& plan, QString* error) {
    std::vector<const wt::session::SessionProfile*> chain;
    if (const auto status = context->sessions().jumpChain(profile, chain); !status) {
        if (error) {
            *error = QString::fromStdString(status.message());
        }
        return false;
    }
    plan = {};
    // The chain lists the nearest jump host first; connect from the outermost one inwards.
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        wt::ssh::ConnectOptions hop;
        if (!hopFromProfile(**it, context, dialogParent, hop, error)) {
            return false;
        }
        plan.hops.push_back(std::move(hop));
    }
    wt::ssh::ConnectOptions target;
    if (!hopFromProfile(profile, context, dialogParent, target, error)) {
        return false;
    }
    if (!plan.hops.empty()) {
        target.proxy = {}; // only the first hop uses a network proxy
    }
    plan.hops.push_back(std::move(target));
    plan.shell.command = profile.ssh.remoteCommand;
    plan.shell.environment = profile.ssh.environment;
    for (const auto& forward : profile.ssh.forwards) {
        if (forward.enabled) {
            plan.shell.forwarding.ports.push_back(toSshForward(forward));
        }
    }
    plan.shell.forwarding.agent = profile.ssh.forwardAgent;
    plan.shell.forwarding.x11 = profile.ssh.forwardX11;
    plan.shell.forwarding.x11Display = profile.ssh.x11Display;
    plan.shell.environment.emplace_back("COLORTERM", "truecolor");
    plan.shell.environment.emplace_back("TERM_PROGRAM", "WindTerm-Weifan");
    const auto& last = plan.hops.back();
    plan.description = QString::fromStdString(last.username + "@" + last.host) +
                       (last.port != 22 ? QStringLiteral(":%1").arg(last.port) : QString());
    return true;
}

std::shared_ptr<wt::ssh::SshConnection> connectPlan(SshPlan& plan, wt::ssh::SshInteraction& ui, std::string* error,
                                                    wt::ssh::Aborter* aborter) {
    std::shared_ptr<wt::ssh::SshConnection> connection;
    for (std::size_t i = 0; i < plan.hops.size(); ++i) {
        wt::ssh::ConnectOptions& hop = plan.hops[i];
        std::string hopError;
        auto next = wt::ssh::SshConnection::connect(hop, ui, &hopError, connection, aborter);
        if (!next) {
            if (error) {
                *error = i + 1 < plan.hops.size() ? "jump host " + hop.host + ": " + hopError : hopError;
            }
            return nullptr;
        }
        // Remember what worked for reconnects (memory only).
        if (next->usedPassword()) {
            hop.password = next->usedPassword();
        }
        if (next->usedPassphrase()) {
            hop.passphrase = next->usedPassphrase();
        }
        connection = std::move(next);
    }
    return connection;
}

// ---------------------------------------------------------------------------------------------
// SshBackend

struct SshBackend::Attempt {
    std::atomic<bool> cancelled{false};
    std::atomic<bool> done{false}; // the worker thread is about to return
    wt::ssh::Aborter aborter;
};

namespace {
// A connection only counts as "working" (and resets the reconnect backoff) after this long.
constexpr qint64 kStableConnectionMs = 10000;
// How long teardown waits for a cancelled worker before leaving it to finish on its own.
constexpr int kWorkerJoinMs = 300;
} // namespace

SshBackend::SshBackend(SshPlan plan, wt::session::ReconnectSettings reconnect, QWidget* dialogParent, QObject* parent)
    : SessionBackend(parent),
      m_plan(std::move(plan)),
      m_backoff(reconnect),
      m_dialogParent(dialogParent),
      m_prompter(SshPrompter::create(dialogParent)) {
    m_reconnectTimer.setSingleShot(true);
    connect(&m_reconnectTimer, &QTimer::timeout, this, &SshBackend::startAttempt);
}

SshBackend::~SshBackend() {
    terminate();
}

void SshBackend::setState(State state) {
    if (m_state != state) {
        m_state = state;
        Q_EMIT stateChanged(state);
    }
}

bool SshBackend::start(int rows, int columns, QString* error) {
    if (m_plan.hops.empty()) {
        if (error) {
            *error = tr("Nothing to connect to");
        }
        return false;
    }
    m_plan.shell.rows = rows;
    m_plan.shell.columns = columns;
    startAttempt();
    return true;
}

void SshBackend::joinWorker() {
    if (m_attempt) {
        m_attempt->cancelled = true;
        m_attempt->aborter.abort();
    }
    if (!m_worker.joinable()) {
        return;
    }
    // Name resolution cannot be interrupted: do not freeze the GUI waiting for it. A detached
    // worker only owns shared state and reports through a guarded pointer, so it may outlive us.
    if (m_attempt) {
        for (int waited = 0; waited < kWorkerJoinMs && !m_attempt->done.load(); waited += 10) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (!m_attempt->done.load()) {
            m_worker.detach();
            return;
        }
    }
    m_worker.join();
}

void SshBackend::startAttempt() {
    if (m_state == State::Connecting || m_state == State::Connected) {
        return;
    }
    joinWorker(); // a finished previous attempt
    setState(State::Connecting);
    Q_EMIT notice(tr("Connecting to %1...").arg(m_plan.description));
    auto attempt = std::make_shared<Attempt>();
    m_attempt = attempt;
    SshPlan plan = m_plan;
    QPointer<SshBackend> self(this);
    std::shared_ptr<SshPrompter> prompter = m_prompter;
    m_worker = std::thread([attempt, plan = std::move(plan), self, prompter]() mutable {
        GuiInteraction ui(prompter, &attempt->cancelled, [self, attempt](const QString& message) {
            if (message.startsWith(QLatin1String("warning")) || message.contains(QLatin1String("Permission denied")) ||
                message.contains(QLatin1String("wrong passphrase"))) {
                QMetaObject::invokeMethod(QCoreApplication::instance(), [self, message] {
                    if (SshBackend* backend = self.data()) {
                        Q_EMIT backend->notice(message);
                    }
                });
            }
        });
        std::string error;
        std::shared_ptr<wt::ssh::SshShell> shell;
        auto connection = connectPlan(plan, ui, &error, &attempt->aborter);
        if (connection && !attempt->cancelled) {
            wt::ssh::SshShell::Callbacks callbacks;
            callbacks.onData = [self](std::string_view data) {
                const QByteArray bytes(data.data(), static_cast<qsizetype>(data.size()));
                QMetaObject::invokeMethod(QCoreApplication::instance(), [self, bytes] {
                    if (SshBackend* backend = self.data()) {
                        Q_EMIT backend->dataReceived(bytes);
                    }
                });
            };
            callbacks.onClosed = [self](int status, const std::string& message) {
                const QString text = QString::fromStdString(message);
                QMetaObject::invokeMethod(QCoreApplication::instance(), [self, status, text] {
                    if (SshBackend* backend = self.data()) {
                        backend->onShellClosed(status, text);
                    }
                });
            };
            callbacks.onNotice = [self](const std::string& message) {
                const QString text = QStringLiteral("[%1]").arg(QString::fromStdString(message));
                QMetaObject::invokeMethod(QCoreApplication::instance(), [self, text] {
                    if (SshBackend* backend = self.data()) {
                        Q_EMIT backend->notice(text);
                        Q_EMIT backend->forwardsChanged();
                    }
                });
            };
            auto started = wt::ssh::SshShell::start(connection, plan.shell, std::move(callbacks), &error);
            shell = std::shared_ptr<wt::ssh::SshShell>(std::move(started));
        }
        connection.reset();
        attempt->aborter.detach(); // setup is over; the socket may close (and be reused) any time now
        if (attempt->cancelled) {
            attempt->done = true;
            return;
        }
        const QString message = QString::fromStdString(error);
        QMetaObject::invokeMethod(QCoreApplication::instance(), [self, shell, plan, message, attempt]() mutable {
            SshBackend* backend = self.data();
            if (backend && backend->m_attempt == attempt) {
                backend->onAttemptDone(std::move(shell), std::move(plan), message);
            } else if (shell) {
                shell->close();
            }
        });
        attempt->done = true;
    });
}

void SshBackend::onAttemptDone(std::shared_ptr<wt::ssh::SshShell> shell, SshPlan plan, const QString& error) {
    m_attempt.reset(); // its aborter must not touch sockets any more
    if (m_state != State::Connecting) {
        if (shell) {
            shell->close();
        }
        return;
    }
    if (!shell) {
        setState(State::Idle);
        const QString message = error.isEmpty() ? tr("Connection failed") : error;
        if ((m_autoReconnecting || m_everConnected) && !isPermanentFailure(message) && scheduleReconnect(message)) {
            return;
        }
        finishWith(-1, message);
        return;
    }
    // Keep the secrets that worked, plus the current window size.
    const int rows = m_plan.shell.rows;
    const int columns = m_plan.shell.columns;
    const int pixelWidth = m_plan.shell.pixelWidth;
    const int pixelHeight = m_plan.shell.pixelHeight;
    m_plan = std::move(plan);
    m_shell = std::move(shell);
    if (rows != m_plan.shell.rows || columns != m_plan.shell.columns) {
        m_shell->resize(rows, columns, pixelWidth, pixelHeight);
    }
    m_plan.shell.rows = rows;
    m_plan.shell.columns = columns;
    if (m_autoReconnecting || m_everConnected) {
        Q_EMIT notice(tr("[Reconnected to %1]").arg(m_plan.description));
    }
    m_everConnected = true;
    m_autoReconnecting = false;
    m_connectedSince.start();
    setState(State::Connected);
    // The shell may have ended before we adopted it (e.g. a remote command that exits at once);
    // its onClosed notification was ignored while connecting.
    if (m_shell->hasFinished()) {
        handleShellEnd(m_shell->exitStatus(), QString::fromStdString(m_shell->closeError()));
    }
}

void SshBackend::onShellClosed(int exitStatus, const QString& error) {
    if (m_state != State::Connected || !m_shell) {
        return; // stale, or handled by onAttemptDone()
    }
    handleShellEnd(exitStatus, error);
}

void SshBackend::handleShellEnd(int exitStatus, const QString& error) {
    if (m_shell) {
        m_shell->close();
        m_shell.reset();
    }
    setState(State::Idle);
    if (error.isEmpty()) {
        finishWith(exitStatus, QString()); // the remote shell exited normally
        return;
    }
    // Only a connection that worked for a while earns a fresh set of attempts; one that drops
    // right after logging in (session limits, forced commands) must not reconnect forever.
    if (m_connectedSince.isValid() && m_connectedSince.elapsed() >= kStableConnectionMs) {
        m_backoff.reset();
    }
    if (!scheduleReconnect(error)) {
        finishWith(-1, error);
    }
}

bool SshBackend::scheduleReconnect(const QString& reason) {
    const auto delay = m_backoff.next();
    if (!delay) {
        return false;
    }
    m_autoReconnecting = true;
    setState(State::WaitingToReconnect);
    Q_EMIT notice(tr("[%1. Reconnecting in %2 s (attempt %3 of %4)...]")
                      .arg(reason)
                      .arg(static_cast<double>(delay->count()) / 1000.0, 0, 'f', 1)
                      .arg(m_backoff.attempts())
                      .arg(m_backoff.maxAttempts()));
    m_reconnectTimer.start(static_cast<int>(delay->count()));
    return true;
}

void SshBackend::finishWith(int exitCode, const QString& message) {
    m_reconnectTimer.stop();
    m_autoReconnecting = false;
    setState(State::Finished);
    if (!message.isEmpty()) {
        Q_EMIT notice(QStringLiteral("[%1]").arg(message));
    }
    Q_EMIT finished(exitCode);
}

void SshBackend::reconnect() {
    if (m_state == State::Connected || m_state == State::Connecting) {
        return;
    }
    m_reconnectTimer.stop();
    m_backoff.reset();
    m_autoReconnecting = false;
    setState(State::Idle);
    startAttempt();
}

void SshBackend::write(std::string_view bytes) {
    if (m_shell && m_state == State::Connected) {
        m_shell->write(bytes);
    }
}

void SshBackend::resize(int rows, int columns, int pixelWidth, int pixelHeight) {
    m_plan.shell.rows = rows;
    m_plan.shell.columns = columns;
    m_plan.shell.pixelWidth = pixelWidth;
    m_plan.shell.pixelHeight = pixelHeight;
    if (m_shell) {
        m_shell->resize(rows, columns, pixelWidth, pixelHeight);
    }
}

std::vector<wt::ssh::ForwardStatus> SshBackend::forwards() const {
    return m_shell ? m_shell->forwards() : std::vector<wt::ssh::ForwardStatus>{};
}

int SshBackend::addForward(const wt::ssh::PortForward& forward) {
    m_plan.shell.forwarding.ports.push_back(forward); // re-established after reconnects
    const int id = m_shell ? m_shell->addForward(forward) : 0;
    Q_EMIT forwardsChanged();
    return id;
}

void SshBackend::removeForward(int id) {
    if (!m_shell) {
        return;
    }
    for (const auto& status : m_shell->forwards()) {
        if (status.id == id) {
            auto& ports = m_plan.shell.forwarding.ports;
            const auto it = std::find(ports.begin(), ports.end(), status.forward);
            if (it != ports.end()) {
                ports.erase(it);
            }
            break;
        }
    }
    m_shell->removeForward(id);
    Q_EMIT forwardsChanged();
}

bool SshBackend::agentForwardingActive() const {
    return m_shell && m_shell->agentForwarding();
}

bool SshBackend::x11ForwardingActive() const {
    return m_shell && m_shell->x11Forwarding();
}

void SshBackend::terminate() {
    m_reconnectTimer.stop();
    joinWorker();
    m_attempt.reset();
    if (m_shell) {
        m_shell->close();
        m_shell.reset();
    }
    m_state = State::Finished;
}
