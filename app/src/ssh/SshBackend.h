#pragma once

#include "SessionBackend.h"
#include "SshPrompter.h"

#include "wt/session/Backoff.h"
#include "wt/session/SessionProfile.h"
#include "wt/ssh/SshConnection.h"
#include "wt/ssh/SshShell.h"

#include <QElapsedTimer>
#include <QPointer>
#include <QTimer>

#include <memory>
#include <thread>
#include <vector>

class AppContext;

// Everything needed to (re)connect: options for every hop (outermost jump host first, the
// target last) and the shell to start.
struct SshPlan {
    std::vector<wt::ssh::ConnectOptions> hops;
    wt::ssh::ShellOptions shell;
    QString description; // "user@host"

    // Builds a plan for `profile`, resolving jump hosts and stored credentials (which may unlock
    // the vault). Returns false with `error` if the profile cannot be used.
    static bool fromProfile(const wt::session::SessionProfile& profile, AppContext* context, QWidget* dialogParent,
                            SshPlan& plan, QString* error);
};

// Connects every hop of `plan` in turn on the calling thread; records the secrets that worked in
// `plan` (so reconnects and SFTP do not ask again).
std::shared_ptr<wt::ssh::SshConnection> connectPlan(SshPlan& plan, wt::ssh::SshInteraction& ui, std::string* error,
                                                    wt::ssh::Aborter* aborter);

// Remote shell over SSH. Connecting happens on a worker thread (questions are asked through an
// SshPrompter on the GUI thread); the shell itself runs on wt::ssh::SshShell's I/O thread.
// Dropped connections are re-established with exponential backoff.
class SshBackend final : public SessionBackend {
    Q_OBJECT

public:
    enum class State { Idle, Connecting, Connected, WaitingToReconnect, Finished };

    SshBackend(SshPlan plan, wt::session::ReconnectSettings reconnect, QWidget* dialogParent,
               QObject* parent = nullptr);
    ~SshBackend() override;

    bool start(int rows, int columns, QString* error) override;
    void write(std::string_view bytes) override;
    void resize(int rows, int columns, int pixelWidth, int pixelHeight) override;
    void terminate() override;
    bool isRunning() const override { return m_state != State::Finished && m_state != State::Idle; }
    bool canReconnect() const override { return true; }
    void reconnect() override;
    QString description() const override { return m_plan.description; }

    State state() const { return m_state; }
    // The plan including the secrets that authenticated (for an SFTP connection to the same host).
    SshPlan currentPlan() const { return m_plan; }
    int reconnectAttempts() const { return m_backoff.attempts(); }

Q_SIGNALS:
    void stateChanged(SshBackend::State state);

private:
    struct Attempt;
    void setState(State state);
    void startAttempt();
    void onAttemptDone(std::shared_ptr<wt::ssh::SshShell> shell, SshPlan plan, const QString& error);
    void onShellClosed(int exitStatus, const QString& error);
    void handleShellEnd(int exitStatus, const QString& error);
    bool scheduleReconnect(const QString& reason);
    void finishWith(int exitCode, const QString& message);
    void joinWorker();

    SshPlan m_plan;
    wt::session::Backoff m_backoff;
    QPointer<QWidget> m_dialogParent;
    std::shared_ptr<SshPrompter> m_prompter;
    std::shared_ptr<Attempt> m_attempt;
    std::thread m_worker;
    std::shared_ptr<wt::ssh::SshShell> m_shell;
    QTimer m_reconnectTimer;
    QElapsedTimer m_connectedSince;
    State m_state = State::Idle;
    bool m_everConnected = false;
    bool m_autoReconnecting = false;
};
