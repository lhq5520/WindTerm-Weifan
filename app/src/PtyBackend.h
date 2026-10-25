#pragma once

#include "SessionBackend.h"

#include "wt/platform/Pty.h"

#include <QStringList>
#include <QTimer>

#include <chrono>
#include <memory>
#include <optional>
#include <string>

class QSocketNotifier;
class QWinEventNotifier;

// Local process on a pseudo-terminal, integrated with the Qt event loop through socket notifiers
// (POSIX) or an event notifier on the ConPTY output event (Windows).
class PtyBackend final : public SessionBackend {
    Q_OBJECT

public:
    struct Options {
        QString program; // empty: default shell
        QStringList arguments;
        QString workingDirectory;
        bool loginShell = false;
        QList<QPair<QString, QString>> environment;
    };

    explicit PtyBackend(Options options, QObject* parent = nullptr);
    ~PtyBackend() override;

    bool start(int rows, int columns, QString* error) override;
    void write(std::string_view bytes) override;
    void resize(int rows, int columns, int pixelWidth, int pixelHeight) override;
    void terminate() override;
    bool isRunning() const override { return m_pty && !m_finished; }
    QString description() const override;

    std::optional<int> exitCode() const { return m_exitCode; }
    std::int64_t processId() const { return m_pty ? m_pty->processId() : -1; }

private:
    void onReadable();
    void flushWrites();
    void checkExit();
    // Reads whatever is available; returns false once the PTY is closed.
    bool drain(std::size_t budget);
    // The slave side hung up: wait (without blocking the event loop) for the child to exit.
    void onOutputClosed();
    void finish();

    Options m_options;
    std::unique_ptr<wt::platform::Pty> m_pty;
    QSocketNotifier* m_readNotifier = nullptr;
    QSocketNotifier* m_writeNotifier = nullptr;
    QWinEventNotifier* m_eventNotifier = nullptr; // Windows only
    QTimer m_exitTimer;
    std::string m_pendingWrite;
    std::optional<int> m_exitCode;
    bool m_finished = false;
    bool m_outputClosed = false;
    std::chrono::steady_clock::time_point m_killDeadline;
};
