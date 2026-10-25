#include "PtyBackend.h"

#include <QSocketNotifier>
#ifdef Q_OS_WIN
#include <QWinEventNotifier>
#endif

#include <algorithm>
#include <cstdint>
#include <cstdlib>

using namespace std::chrono_literals;
using wt::platform::IoState;

namespace {

constexpr std::size_t kReadChunk = 16 * 1024;
// Upper bound per event-loop iteration so a flood of output cannot starve painting and input.
constexpr std::size_t kReadBudget = 256 * 1024;
// After the child exits, read what it left behind, but do not let a surviving background job that
// keeps writing hold the session open.
constexpr std::size_t kFinalReadBudget = 4 * kReadBudget;
constexpr auto kExitPollInterval = 200ms;
constexpr auto kClosedPollInterval = 20ms;
// How long a child may linger after closing the terminal before it is sent SIGHUP/SIGKILL.
constexpr auto kHangupGrace = 500ms;

} // namespace

PtyBackend::PtyBackend(Options options, QObject* parent) : SessionBackend(parent), m_options(std::move(options)) {
    m_exitTimer.setInterval(kExitPollInterval);
    connect(&m_exitTimer, &QTimer::timeout, this, &PtyBackend::checkExit);
}

PtyBackend::~PtyBackend() {
    terminate();
}

QString PtyBackend::description() const {
    return m_options.program.isEmpty() ? QString::fromStdString(wt::platform::defaultShell()) : m_options.program;
}

bool PtyBackend::start(int rows, int columns, QString* error) {
    if (m_pty) {
        if (error) {
            *error = tr("Session already started");
        }
        return false;
    }
    wt::platform::PtyOptions ptyOptions;
    ptyOptions.program = m_options.program.toStdString();
    for (const QString& arg : m_options.arguments) {
        ptyOptions.arguments.push_back(arg.toStdString());
    }
    ptyOptions.workingDirectory = m_options.workingDirectory.toStdString();
    ptyOptions.loginShell = m_options.loginShell;
    ptyOptions.size.rows = static_cast<std::uint16_t>(std::clamp(rows, 1, 0xFFFF));
    ptyOptions.size.columns = static_cast<std::uint16_t>(std::clamp(columns, 1, 0xFFFF));
    ptyOptions.environment = {
        {"TERM", "xterm-256color"},
        {"COLORTERM", "truecolor"},
        {"TERM_PROGRAM", "WindTerm-Weifan"},
    };
    // GUI apps launched from Finder/Dock get no locale; shells then mangle UTF-8 input.
    if (!std::getenv("LANG") && !std::getenv("LC_ALL") && !std::getenv("LC_CTYPE")) {
        ptyOptions.environment.emplace_back("LANG", "en_US.UTF-8");
    }
    for (const auto& [name, value] : m_options.environment) {
        ptyOptions.environment.emplace_back(name.toStdString(), value.toStdString());
    }

    auto pty = wt::platform::createPty();
    if (const auto status = pty->start(ptyOptions); !status) {
        if (error) {
            *error = QString::fromStdString(status.message());
        }
        return false;
    }
    m_pty = std::move(pty);
    m_finished = false;
    m_outputClosed = false;
    m_exitCode.reset();

#ifdef Q_OS_WIN
    m_eventNotifier = new QWinEventNotifier(reinterpret_cast<Qt::HANDLE>(m_pty->nativeHandle()), this);
    connect(m_eventNotifier, &QWinEventNotifier::activated, this, &PtyBackend::onReadable);
#else
    const auto fd = static_cast<qintptr>(m_pty->nativeHandle());
    m_readNotifier = new QSocketNotifier(fd, QSocketNotifier::Read, this);
    connect(m_readNotifier, &QSocketNotifier::activated, this, &PtyBackend::onReadable);
    m_writeNotifier = new QSocketNotifier(fd, QSocketNotifier::Write, this);
    m_writeNotifier->setEnabled(false);
    connect(m_writeNotifier, &QSocketNotifier::activated, this, &PtyBackend::flushWrites);
#endif
    m_exitTimer.start();
    return true;
}

void PtyBackend::write(std::string_view bytes) {
    if (!m_pty || m_finished || bytes.empty()) {
        return;
    }
    m_pendingWrite.append(bytes);
    flushWrites();
}

void PtyBackend::flushWrites() {
    while (m_pty && !m_pendingWrite.empty()) {
        const auto result = m_pty->write(m_pendingWrite);
        if (result.state == IoState::Ok) {
            m_pendingWrite.erase(0, result.bytes);
        } else if (result.state == IoState::WouldBlock) {
            break;
        } else {
            m_pendingWrite.clear();
        }
    }
    if (m_writeNotifier) {
        m_writeNotifier->setEnabled(!m_pendingWrite.empty());
    }
}

void PtyBackend::resize(int rows, int columns, int pixelWidth, int pixelHeight) {
    if (!m_pty || m_finished) {
        return;
    }
    auto clamp16 = [](int value) { return static_cast<std::uint16_t>(std::clamp(value, 0, 0xFFFF)); };
    m_pty->resize({clamp16(rows), clamp16(columns), clamp16(pixelWidth), clamp16(pixelHeight)});
}

bool PtyBackend::drain(std::size_t budget) {
    char buffer[kReadChunk];
    std::size_t total = 0;
    bool open = true;
    while (total < budget) {
        const auto result = m_pty->read(buffer, sizeof(buffer));
        if (result.state == IoState::Ok) {
            Q_EMIT dataReceived(QByteArray::fromRawData(buffer, static_cast<qsizetype>(result.bytes)));
            total += result.bytes;
            continue;
        }
        open = result.state == IoState::WouldBlock;
        break;
    }
    return open;
}

void PtyBackend::onReadable() {
    if (!m_pty || m_finished || m_outputClosed) {
        return;
    }
    if (!drain(kReadBudget)) {
        onOutputClosed();
    }
}

void PtyBackend::onOutputClosed() {
    m_outputClosed = true;
    // A hung-up descriptor stays readable; stop watching it or the event loop spins.
#ifdef Q_OS_WIN
    if (m_eventNotifier) {
        m_eventNotifier->setEnabled(false);
    }
#endif
    if (m_readNotifier) {
        m_readNotifier->setEnabled(false);
    }
    if (m_writeNotifier) {
        m_writeNotifier->setEnabled(false);
    }
    m_pendingWrite.clear();
    if (m_pty->pollExit()) {
        finish();
        return;
    }
    // Usually the child exits right after closing the terminal; poll for it briefly instead of
    // blocking the event loop, then hang it up.
    m_killDeadline = std::chrono::steady_clock::now() + kHangupGrace;
    m_exitTimer.start(kClosedPollInterval);
}

void PtyBackend::checkExit() {
    if (!m_pty || m_finished) {
        return;
    }
    if (m_pty->pollExit()) {
        if (!m_outputClosed) {
            // Output written just before exit may still be buffered in the PTY.
            drain(kFinalReadBudget);
        }
        finish();
    } else if (m_outputClosed && std::chrono::steady_clock::now() >= m_killDeadline) {
        m_pty->terminate(100ms);
        finish();
    }
}

void PtyBackend::finish() {
    if (m_finished) {
        return;
    }
    m_finished = true;
    m_exitTimer.stop();
#ifdef Q_OS_WIN
    if (m_eventNotifier) {
        m_eventNotifier->setEnabled(false);
    }
#endif
    if (m_readNotifier) {
        m_readNotifier->setEnabled(false);
    }
    if (m_writeNotifier) {
        m_writeNotifier->setEnabled(false);
    }
    m_pendingWrite.clear();
    m_exitCode = m_pty->pollExit();
    Q_EMIT finished(m_exitCode.value_or(-1));
}

void PtyBackend::terminate() {
    if (!m_pty) {
        return;
    }
    // Drop the notifiers before the PTY closes the descriptor they watch.
#ifdef Q_OS_WIN
    delete m_eventNotifier;
    m_eventNotifier = nullptr;
#endif
    delete m_readNotifier;
    m_readNotifier = nullptr;
    delete m_writeNotifier;
    m_writeNotifier = nullptr;
    m_exitTimer.stop();
    m_pty->terminate();
    if (!m_finished) {
        m_finished = true;
        m_exitCode = m_pty->pollExit();
    }
}
