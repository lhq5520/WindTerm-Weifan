#include "TerminalPane.h"

#include "AppContext.h"
#include "PtyBackend.h"
#include "SearchBar.h"
#include "TerminalWidget.h"

#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QVBoxLayout>

TerminalPane::TerminalPane(AppContext* context, QWidget* parent)
    : QWidget(parent),
      m_context(context),
      m_session(new TerminalSession(this)),
      m_terminal(new TerminalWidget(this)),
      m_searchBar(nullptr) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    m_terminal->setSession(m_session);
    m_searchBar = new SearchBar(m_terminal, this);
    m_searchBar->hide();
    layout->addWidget(m_searchBar);
    layout->addWidget(m_terminal, 1);
    setFocusProxy(m_terminal);

    connect(m_searchBar, &SearchBar::closed, m_terminal, [this] { m_terminal->setFocus(); });
    connect(m_terminal, &TerminalWidget::focusReceived, this, [this] { Q_EMIT activated(this); });
    connect(m_terminal, &TerminalWidget::bellRang, this, [this] { Q_EMIT bellRang(this); });
    wireSession();
    applySettings();
}

TerminalPane::~TerminalPane() {
    m_session->terminate();
}

void TerminalPane::wireSession() {
    connect(m_session, &TerminalSession::titleChanged, this, &TerminalPane::titleChanged);
    connect(m_session, &TerminalSession::finished, this, [this](int code) { Q_EMIT sessionFinished(this, code); });
    connect(m_session->triggers(), &TriggerRunner::notifyRequested, this,
            [this](const QString& title, const QString& message) { Q_EMIT triggerNotification(this, title, message); });
    connect(m_session->triggers(), &TriggerRunner::beepRequested, this, [] { QApplication::beep(); });
    connect(m_terminal, &TerminalWidget::commandSubmitted, this, [this](const QString& command) {
        if (m_context && m_context->settings().completion.enabled) {
            m_context->recordCommand(historyContext(), command);
        }
    });
}

QString TerminalPane::historyContext() const {
    return m_profile ? QString::fromStdString(m_profile->id) : QStringLiteral("local");
}

QString TerminalPane::hostName() const {
    if (!m_profile) {
        return QStringLiteral("local");
    }
    switch (m_profile->kind) {
    case wt::session::SessionKind::Ssh: return QString::fromStdString(m_profile->ssh.host);
    case wt::session::SessionKind::Telnet: return QString::fromStdString(m_profile->telnet.host);
    case wt::session::SessionKind::Serial:
        return QFileInfo(QString::fromStdString(m_profile->serial.line.device)).fileName();
    case wt::session::SessionKind::Local: break;
    }
    return QStringLiteral("local");
}

bool TerminalPane::startLogging(QString* error) {
    if (!m_context) {
        if (error) {
            *error = tr("No settings to take the log directory from");
        }
        return false;
    }
    const auto& logging = m_context->settings().logging;
    return m_session->startLog(m_context->logPathFor(title(), hostName()), logging.format, logging.timestamps, error);
}

void TerminalPane::stopLogging() {
    m_session->stopLog();
}

bool TerminalPane::isRemote() const {
    return m_profile && m_profile->isRemote();
}

wt::session::PaneState TerminalPane::paneState() const {
    wt::session::PaneState state;
    state.title = title().toStdString();
    if (m_profile) {
        if (m_context && m_context->sessions().find(m_profile->id)) {
            state.profileId = m_profile->id;
        } else {
            state.profileJson = m_profile->toJson().dump(); // references secrets by id only
        }
        if (m_profile->kind != wt::session::SessionKind::Local) {
            return state;
        }
    } else {
        state.program = m_localOptions.program.toStdString();
        for (const QString& argument : m_localOptions.arguments) {
            state.arguments.push_back(argument.toStdString());
        }
    }
    const std::string& cwd = m_session->terminal().workingDirectory();
    state.workingDirectory = !cwd.empty() ? cwd : m_localOptions.workingDirectory.toStdString();
    return state;
}

bool TerminalPane::startLocal(const TerminalSession::Options& options) {
    m_localOptions = options;
    m_fallbackTitle = options.program.isEmpty() ? QFileInfo(QString::fromStdString(wt::platform::defaultShell())).fileName()
                                                : QFileInfo(options.program).fileName();
    if (!m_session->start(options)) {
        return false;
    }
    if (m_terminal->isVisible()) {
        m_terminal->syncGridSize(); // a hidden view resizes the session when it is shown
    }
    return true;
}

bool TerminalPane::startProfile(const wt::session::SessionProfile& profile) {
    m_profile = profile;
    applySettings(); // the profile may override the theme
    if (profile.kind == wt::session::SessionKind::Local) {
        TerminalSession::Options options;
        options.program = QString::fromStdString(profile.local.program);
        for (const auto& arg : profile.local.arguments) {
            options.arguments << QString::fromStdString(arg);
        }
        options.workingDirectory = profile.local.workingDirectory.empty()
                                       ? QDir::homePath()
                                       : QString::fromStdString(profile.local.workingDirectory);
        options.loginShell = profile.local.loginShell;
        for (const auto& [name, value] : profile.local.environment) {
            options.environment.append({QString::fromStdString(name), QString::fromStdString(value)});
        }
        const bool ok = startLocal(options);
        m_fallbackTitle = QString::fromStdString(profile.name);
        return ok;
    }
    m_fallbackTitle = QString::fromStdString(profile.name);
    return false; // SSH backends are created by the window (see MainWindow::openProfile)
}

QString TerminalPane::title() const {
    const QString title = QString::fromStdString(m_session->terminal().title());
    if (!title.isEmpty()) {
        return title;
    }
    return m_fallbackTitle.isEmpty() ? tr("Terminal") : m_fallbackTitle;
}

void TerminalPane::showSearch() {
    m_searchBar->activate();
}

void TerminalPane::findNext() {
    if (m_searchBar->isHidden() || m_searchBar->query().isEmpty()) {
        showSearch();
        return;
    }
    m_searchBar->findNext();
}

void TerminalPane::findPrevious() {
    if (m_searchBar->isHidden() || m_searchBar->query().isEmpty()) {
        showSearch();
        return;
    }
    m_searchBar->findPrevious();
}

void TerminalPane::applySettings() {
    if (!m_context) {
        return;
    }
    const auto& settings = m_context->settings();
    const QString themeOverride = m_profile ? QString::fromStdString(m_profile->theme) : QString();
    m_terminal->setAppearance(TerminalAppearance::fromSettings(settings, themeOverride));
    m_session->terminal().setScrollbackLimit(static_cast<std::size_t>(settings.terminal.scrollbackLines));
    m_session->setTriggers(m_context->triggers());
    m_terminal->setHighlightRules(m_context->triggers());
    if (settings.completion.enabled) {
        AppContext* context = m_context;
        m_terminal->setCompletionProvider(
            [this, context](const QString& prefix) { return context->completeCommand(historyContext(), prefix); },
            settings.completion.minimumPrefix);
    } else {
        m_terminal->setCompletionProvider({});
    }
}
