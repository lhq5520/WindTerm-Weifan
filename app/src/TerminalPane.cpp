#include "TerminalPane.h"

#include "AppContext.h"
#include "PtyBackend.h"
#include "SearchBar.h"
#include "TerminalWidget.h"

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
}

bool TerminalPane::isRemote() const {
    return m_profile && m_profile->isRemote();
}

bool TerminalPane::startLocal(const TerminalSession::Options& options) {
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
}
