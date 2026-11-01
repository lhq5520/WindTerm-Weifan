#include "SerialBackend.h"

#include <QSocketNotifier>

#include <cstring>
#ifdef Q_OS_WIN
#include <QWinEventNotifier>
#endif

using wt::platform::IoState;

namespace {

constexpr std::size_t kReadBudget = 256 * 1024;

} // namespace

SerialBackend::SerialBackend(Options options, QObject* parent)
    : SessionBackend(parent), m_options(std::move(options)), m_backoff(m_options.reconnect) {
    m_reopenTimer.setSingleShot(true);
    connect(&m_reopenTimer, &QTimer::timeout, this, &SerialBackend::tryReopen);
    m_writeRetry.setInterval(20);
    connect(&m_writeRetry, &QTimer::timeout, this, &SerialBackend::flushWrites);
}

SerialBackend::~SerialBackend() {
    terminate();
}

SerialBackend::Options SerialBackend::fromProfile(const wt::session::SessionProfile& profile) {
    Options options;
    options.line = profile.serial.line;
    options.localEcho = profile.serial.localEcho;
    options.newline = profile.serial.newline;
    options.reconnect = profile.serial.reconnect;
    return options;
}

QString SerialBackend::description() const {
    return QStringLiteral("%1 (%2)")
        .arg(QString::fromStdString(m_options.line.device),
             QString::fromStdString(wt::platform::describeSerialConfig(m_options.line)));
}

bool SerialBackend::start(int rows, int columns, QString* error) {
    Q_UNUSED(rows);
    Q_UNUSED(columns);
    m_finished = false;
    if (!open(error)) {
        return false;
    }
    Q_EMIT notice(tr("Connected to %1").arg(description()));
    return true;
}

bool SerialBackend::open(QString* error) {
    wt::core::Status status;
    auto port = wt::platform::SerialPort::open(m_options.line, &status);
    if (!port) {
        if (error) {
            *error = QString::fromStdString(status.message());
        }
        return false;
    }
    m_port = std::move(port);
#ifdef Q_OS_WIN
    m_eventNotifier = new QWinEventNotifier(reinterpret_cast<Qt::HANDLE>(m_port->nativeHandle()), this);
    connect(m_eventNotifier, &QWinEventNotifier::activated, this, &SerialBackend::onReadable);
#else
    const auto fd = static_cast<qintptr>(m_port->nativeHandle());
    m_readNotifier = new QSocketNotifier(fd, QSocketNotifier::Read, this);
    connect(m_readNotifier, &QSocketNotifier::activated, this, &SerialBackend::onReadable);
    m_writeNotifier = new QSocketNotifier(fd, QSocketNotifier::Write, this);
    m_writeNotifier->setEnabled(false);
    connect(m_writeNotifier, &QSocketNotifier::activated, this, &SerialBackend::flushWrites);
#endif
    return true;
}

void SerialBackend::closePort() {
    delete m_readNotifier;
    m_readNotifier = nullptr;
    delete m_writeNotifier;
    m_writeNotifier = nullptr;
#ifdef Q_OS_WIN
    delete m_eventNotifier;
    m_eventNotifier = nullptr;
#endif
    m_writeRetry.stop();
    if (m_port) {
        m_port->close();
        m_port.reset();
    }
    m_pendingWrite.clear();
}

void SerialBackend::onReadable() {
    if (!m_port) {
        return;
    }
    char buffer[16384];
    QByteArray output;
    std::size_t total = 0;
    QString gone;
    while (total < kReadBudget) {
        const auto result = m_port->read(buffer, sizeof(buffer));
        if (result.state == IoState::Ok) {
            output.append(buffer, static_cast<qsizetype>(result.bytes));
            total += result.bytes;
            continue;
        }
        if (result.state == IoState::Closed) {
            gone = tr("%1 was disconnected").arg(QString::fromStdString(m_options.line.device));
        } else if (result.state == IoState::Error) {
            gone = tr("Error reading %1: %2")
                       .arg(QString::fromStdString(m_options.line.device),
                            QString::fromLocal8Bit(std::strerror(result.systemError)));
        }
        break;
    }
    if (!output.isEmpty()) {
        Q_EMIT dataReceived(output);
    }
    if (!gone.isEmpty()) {
        onDeviceGone(gone);
    }
}

std::string SerialBackend::translate(std::string_view input) const {
    if (m_options.newline == wt::session::NewlineMode::Cr) {
        return std::string(input);
    }
    std::string out;
    out.reserve(input.size() + 4);
    for (const char c : input) {
        if (c != '\r') {
            out += c;
        } else if (m_options.newline == wt::session::NewlineMode::Lf) {
            out += '\n';
        } else {
            out += "\r\n";
        }
    }
    return out;
}

void SerialBackend::write(std::string_view bytes) {
    if (!m_port || bytes.empty()) {
        return;
    }
    if (m_options.localEcho == wt::session::EchoMode::On && bytes.front() != '\x1b') {
        std::string echo;
        for (const char c : bytes) {
            const auto byte = static_cast<unsigned char>(c);
            if (c == '\r') {
                echo += "\r\n";
            } else if (byte == 0x7F || byte == 0x08) {
                echo += "\b \b";
            } else if (byte >= 0x20 || c == '\t' || c == '\n') {
                echo += c;
            }
        }
        if (!echo.empty()) {
            Q_EMIT dataReceived(QByteArray(echo.data(), static_cast<qsizetype>(echo.size())));
        }
    }
    m_pendingWrite += translate(bytes);
    flushWrites();
}

void SerialBackend::flushWrites() {
    while (m_port && !m_pendingWrite.empty()) {
        const auto result = m_port->write(m_pendingWrite);
        if (result.state == IoState::Ok && result.bytes > 0) {
            m_pendingWrite.erase(0, result.bytes);
        } else if (result.state == IoState::WouldBlock || (result.state == IoState::Ok && result.bytes == 0)) {
            break;
        } else {
            m_pendingWrite.clear(); // the read side reports the failure
        }
    }
    const bool pending = m_port && !m_pendingWrite.empty();
    if (m_writeNotifier) {
        m_writeNotifier->setEnabled(pending);
    } else if (pending && !m_writeRetry.isActive()) {
        m_writeRetry.start();
    } else if (!pending) {
        m_writeRetry.stop();
    }
}

void SerialBackend::onDeviceGone(const QString& reason) {
    closePort();
    m_backoff.reset();
    if (m_options.reconnect.enabled && !m_finished) {
        Q_EMIT notice(tr("[%1. Waiting for the device to come back...]").arg(reason));
        if (const auto delay = m_backoff.next()) {
            m_reopenTimer.start(static_cast<int>(delay->count()));
            return;
        }
    }
    m_finished = true;
    Q_EMIT notice(QStringLiteral("[%1]").arg(reason));
    Q_EMIT finished(-1);
}

void SerialBackend::tryReopen() {
    if (m_finished || m_port) {
        return;
    }
    QString error;
    if (open(&error)) {
        Q_EMIT notice(tr("[Reconnected to %1]").arg(description()));
        return;
    }
    if (const auto delay = m_backoff.next()) {
        m_reopenTimer.start(static_cast<int>(delay->count()));
        return;
    }
    m_finished = true;
    Q_EMIT notice(QStringLiteral("[%1]").arg(error));
    Q_EMIT finished(-1);
}

void SerialBackend::reconnect() {
    if (m_port) {
        return;
    }
    m_reopenTimer.stop();
    m_finished = false;
    QString error;
    if (open(&error)) {
        Q_EMIT notice(tr("[Reconnected to %1]").arg(description()));
    } else {
        m_finished = true;
        Q_EMIT notice(QStringLiteral("[%1]").arg(error));
        Q_EMIT finished(-1);
    }
}

void SerialBackend::sendBreak(int milliseconds) {
    if (!m_port || !m_port->setBreak(true)) {
        return;
    }
    QTimer::singleShot(milliseconds, this, [this] {
        if (m_port) {
            m_port->setBreak(false);
        }
    });
}

bool SerialBackend::reconfigure(const wt::platform::SerialConfig& line, QString* error) {
    wt::platform::SerialConfig updated = line;
    updated.device = m_options.line.device;
    if (m_port) {
        if (const auto status = m_port->reconfigure(updated); !status) {
            if (error) {
                *error = QString::fromStdString(status.message());
            }
            return false;
        }
    }
    m_options.line = updated;
    return true;
}

void SerialBackend::terminate() {
    m_reopenTimer.stop();
    closePort();
    m_finished = true;
}
