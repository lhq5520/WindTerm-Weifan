#pragma once

#include "SessionBackend.h"
#include "Triggers.h"

#include "wt/session/SessionLog.h"
#include "wt/terminal/Terminal.h"

#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>

#include <memory>
#include <optional>
#include <string_view>

// Connects a backend (local PTY or remote shell) to a terminal model on the Qt event loop.
// All methods must be called from the thread that owns the session.
class TerminalSession : public QObject {
    Q_OBJECT

public:
    // Local process options (see PtyBackend).
    struct Options {
        QString program; // empty: default shell
        QStringList arguments;
        QString workingDirectory;
        bool loginShell = false;
        QList<QPair<QString, QString>> environment;
        int rows = 24;
        int columns = 80;
    };

    explicit TerminalSession(QObject* parent = nullptr);
    ~TerminalSession() override;

    // Starts a local process.
    bool start(const Options& options);
    // Starts an arbitrary backend (takes ownership) with the terminal's current size.
    bool start(std::unique_ptr<SessionBackend> backend, int rows = 0, int columns = 0);
    QString errorString() const { return m_errorString; }

    wt::terminal::Terminal& terminal() { return m_terminal; }
    const wt::terminal::Terminal& terminal() const { return m_terminal; }
    SessionBackend* backend() const { return m_backend; }

    void sendBytes(std::string_view bytes);
    void sendText(const QString& text);
    // Input typed or pasted by the user (as opposed to terminal replies): also announced through
    // inputSent() for input broadcasting.
    void sendInput(std::string_view bytes);

    // Session log (see wt::session::SessionLogger).
    bool startLog(const QString& path, wt::session::LogFormat format, bool timestamps, QString* error = nullptr);
    void stopLog();
    bool isLogging() const { return m_log.isOpen(); }
    QString logPath() const { return QString::fromStdString(m_log.path()); }

    // Notify / beep / send triggers (highlights are the view's business).
    void setTriggers(std::vector<CompiledTrigger> triggers) { m_triggers.setRules(std::move(triggers)); }
    TriggerRunner* triggers() { return &m_triggers; }
    // Resizes the terminal model and the backend (SIGWINCH / window-change request). Cell pixel
    // sizes are reported through the PTY window size and XTWINOPS; 0 means unknown.
    void resize(int rows, int columns, int cellPixelWidth = 0, int cellPixelHeight = 0);
    void terminate();
    // Re-establishes a dropped remote connection (no-op for local sessions).
    void reconnect();

    bool isRunning() const { return m_backend && m_backend->isRunning() && !m_finished; }
    bool hasFinished() const { return m_finished; }
    std::optional<int> exitCode() const { return m_exitCode; }

    // Writes informational text into the terminal (e.g. "[connection lost]"), on its own line.
    void showNotice(const QString& text);

Q_SIGNALS:
    void outputReceived();
    void titleChanged(const QString& title);
    void bell();
    void finished(int exitCode);
    // OSC 52 request (only emitted when clipboard writes are allowed).
    void clipboardWriteRequested(char target, const QString& text);
    void inputSent(const QByteArray& bytes);
    void loggingChanged(bool logging);

private:
    void onData(const QByteArray& bytes);
    void onFinished(int exitCode);

    wt::terminal::Terminal m_terminal;
    wt::session::SessionLogger m_log;
    TriggerRunner m_triggers;
    SessionBackend* m_backend = nullptr; // owned (child object)
    QString m_errorString;
    std::optional<int> m_exitCode;
    bool m_finished = false;
    int m_cellPixelWidth = 0;
    int m_cellPixelHeight = 0;
};
