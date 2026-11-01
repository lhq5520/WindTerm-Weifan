#pragma once

#include "SessionBackend.h"

#include "wt/platform/SerialPort.h"
#include "wt/session/Backoff.h"
#include "wt/session/SessionProfile.h"

#include <QTimer>

#include <memory>
#include <string>

class QSocketNotifier;
class QWinEventNotifier;

// Serial console: the port is serviced on the GUI thread through a socket notifier on the device
// descriptor (POSIX) or an event notifier on the port's data event (Windows). When the device
// disappears (USB adapter unplugged, board reset) it is reopened automatically if enabled.
class SerialBackend final : public SessionBackend {
    Q_OBJECT

public:
    struct Options {
        wt::platform::SerialConfig line;
        wt::session::EchoMode localEcho = wt::session::EchoMode::Off;
        wt::session::NewlineMode newline = wt::session::NewlineMode::Cr;
        wt::session::ReconnectSettings reconnect{true, 30, 500, 2000};
    };

    explicit SerialBackend(Options options, QObject* parent = nullptr);
    ~SerialBackend() override;

    static Options fromProfile(const wt::session::SessionProfile& profile);

    bool start(int rows, int columns, QString* error) override;
    void write(std::string_view bytes) override;
    void resize(int, int, int, int) override {} // serial lines have no window size
    void terminate() override;
    bool isRunning() const override { return !m_finished && (m_port || m_reopenTimer.isActive()); }
    bool canReconnect() const override { return true; }
    void reconnect() override;
    QString description() const override;

    bool isOpen() const { return m_port != nullptr; }
    // Holds the line in the break condition for `milliseconds` (asynchronously).
    void sendBreak(int milliseconds = 250);
    // Changes baud rate, framing and flow control of the open port.
    bool reconfigure(const wt::platform::SerialConfig& line, QString* error);
    const Options& options() const { return m_options; }

private:
    bool open(QString* error);
    void closePort();
    void onReadable();
    void flushWrites();
    void onDeviceGone(const QString& reason);
    void tryReopen();
    std::string translate(std::string_view input) const;

    Options m_options;
    std::unique_ptr<wt::platform::SerialPort> m_port;
    QSocketNotifier* m_readNotifier = nullptr;
    QSocketNotifier* m_writeNotifier = nullptr;
    QWinEventNotifier* m_eventNotifier = nullptr;
    QTimer m_writeRetry; // Windows: writes that could not complete
    QTimer m_reopenTimer;
    wt::session::Backoff m_backoff;
    std::string m_pendingWrite;
    bool m_finished = false;
};
