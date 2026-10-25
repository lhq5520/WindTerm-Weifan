#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>

#include <string_view>

// Transport behind a TerminalSession: a local PTY or a remote shell. Lives on the GUI thread;
// implementations that block (SSH) do so on their own worker threads.
class SessionBackend : public QObject {
    Q_OBJECT

public:
    using QObject::QObject;

    // Starts the process / connection. Returns false (with `error`) on immediate failure; later
    // failures are reported through finished().
    virtual bool start(int rows, int columns, QString* error) = 0;
    // May be called before the backend is connected; implementations queue or drop the data.
    virtual void write(std::string_view bytes) = 0;
    virtual void resize(int rows, int columns, int pixelWidth, int pixelHeight) = 0;
    // Ends the session for good (tab closed). Does not emit finished().
    virtual void terminate() = 0;
    virtual bool isRunning() const = 0;
    // Remote sessions can be re-established after the connection dropped.
    virtual bool canReconnect() const { return false; }
    virtual void reconnect() {}
    // Short description for tooltips/status ("/bin/zsh", "alice@example.com").
    virtual QString description() const = 0;

Q_SIGNALS:
    void dataReceived(const QByteArray& bytes);
    // The process exited / the connection closed for good (exit code, -1 if unknown).
    void finished(int exitCode);
    // Informational text the session shows inline (connection progress, reconnect attempts).
    void notice(const QString& text);
};
