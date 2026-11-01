#include "TerminalSession.h"

#include "PtyBackend.h"

#include <algorithm>

TerminalSession::TerminalSession(QObject* parent) : QObject(parent), m_terminal(24, 80) {
    m_terminal.setResponseHandler([this](std::string_view bytes) { sendBytes(bytes); });
    m_terminal.setTitleHandler(
        [this](const std::string& title) { Q_EMIT titleChanged(QString::fromStdString(title)); });
    m_terminal.setBellHandler([this] { Q_EMIT bell(); });
    m_terminal.setClipboardHandler([this](char target, const std::string& text) {
        Q_EMIT clipboardWriteRequested(target, QString::fromStdString(text));
    });
    connect(&m_triggers, &TriggerRunner::sendRequested, this,
            [this](const QByteArray& bytes) { sendBytes(std::string_view(bytes.constData(), static_cast<std::size_t>(bytes.size()))); });
}

TerminalSession::~TerminalSession() {
    terminate();
    delete m_backend; // before QObject deletes children, so no signal reaches a half-destroyed session
    m_backend = nullptr;
}

bool TerminalSession::start(const Options& options) {
    PtyBackend::Options ptyOptions;
    ptyOptions.program = options.program;
    ptyOptions.arguments = options.arguments;
    ptyOptions.workingDirectory = options.workingDirectory;
    ptyOptions.loginShell = options.loginShell;
    ptyOptions.environment = options.environment;
    return start(std::make_unique<PtyBackend>(ptyOptions), options.rows, options.columns);
}

bool TerminalSession::start(std::unique_ptr<SessionBackend> backend, int rows, int columns) {
    if (m_backend) {
        m_errorString = tr("Session already started");
        return false;
    }
    if (rows > 0 && columns > 0) {
        m_terminal.resize(rows, columns);
    }
    backend->setParent(this);
    SessionBackend* raw = backend.release();
    connect(raw, &SessionBackend::dataReceived, this, &TerminalSession::onData);
    connect(raw, &SessionBackend::finished, this, &TerminalSession::onFinished);
    connect(raw, &SessionBackend::notice, this, &TerminalSession::showNotice);
    m_backend = raw;
    m_finished = false;
    m_exitCode.reset();
    QString error;
    if (!raw->start(m_terminal.rows(), m_terminal.columns(), &error)) {
        m_errorString = error;
        m_backend = nullptr;
        delete raw;
        return false;
    }
    return true;
}

void TerminalSession::onData(const QByteArray& bytes) {
    const std::string_view data(bytes.constData(), static_cast<std::size_t>(bytes.size()));
    m_terminal.feed(data);
    m_log.write(data);
    m_triggers.feed(data);
    Q_EMIT outputReceived();
}

void TerminalSession::sendInput(std::string_view bytes) {
    if (!m_backend || m_finished || bytes.empty()) {
        return;
    }
    sendBytes(bytes);
    Q_EMIT inputSent(QByteArray(bytes.data(), static_cast<qsizetype>(bytes.size())));
}

bool TerminalSession::startLog(const QString& path, wt::session::LogFormat format, bool timestamps, QString* error) {
    if (const auto status = m_log.open(path.toStdString(), format, timestamps); !status) {
        if (error) {
            *error = QString::fromStdString(status.message());
        }
        Q_EMIT loggingChanged(false);
        return false;
    }
    Q_EMIT loggingChanged(true);
    return true;
}

void TerminalSession::stopLog() {
    if (m_log.isOpen()) {
        m_log.close();
        Q_EMIT loggingChanged(false);
    }
}

void TerminalSession::onFinished(int exitCode) {
    if (m_finished) {
        return;
    }
    m_finished = true;
    m_exitCode = exitCode;
    Q_EMIT outputReceived();
    Q_EMIT finished(exitCode);
}

void TerminalSession::sendBytes(std::string_view bytes) {
    if (!m_backend || m_finished || bytes.empty()) {
        return;
    }
    m_backend->write(bytes);
}

void TerminalSession::sendText(const QString& text) {
    sendBytes(text.toStdString());
}

void TerminalSession::showNotice(const QString& text) {
    // On a line of its own, dimmed, without disturbing the application's attributes for long.
    std::string bytes = m_terminal.cursor().column > 0 ? "\r\n" : "";
    bytes += "\x1b[0;2m" + text.toStdString() + "\x1b[0m\r\n";
    m_terminal.feed(bytes);
    m_log.note(text.toStdString());
    Q_EMIT outputReceived();
}

void TerminalSession::resize(int rows, int columns, int cellPixelWidth, int cellPixelHeight) {
    rows = std::clamp(rows, 1, 0xFFFF);
    columns = std::clamp(columns, 1, 0xFFFF);
    if (rows == m_terminal.rows() && columns == m_terminal.columns() && cellPixelWidth == m_cellPixelWidth &&
        cellPixelHeight == m_cellPixelHeight) {
        return;
    }
    m_cellPixelWidth = cellPixelWidth;
    m_cellPixelHeight = cellPixelHeight;
    m_terminal.setCellPixelSize(cellPixelWidth, cellPixelHeight);
    m_terminal.resize(rows, columns);
    if (m_backend && !m_finished) {
        m_backend->resize(rows, columns, columns * cellPixelWidth, rows * cellPixelHeight);
    }
    Q_EMIT outputReceived();
}

void TerminalSession::terminate() {
    if (!m_backend) {
        return;
    }
    m_backend->terminate();
    m_finished = true;
}

void TerminalSession::reconnect() {
    if (m_backend && m_backend->canReconnect()) {
        m_finished = false;
        m_exitCode.reset();
        m_backend->reconnect();
    }
}
