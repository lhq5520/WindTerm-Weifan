#include "MainWindow.h"

#include "AppContext.h"
#include "SerialBackend.h"
#include "SessionDialogs.h"
#include "SettingsDialog.h"
#include "TelnetBackend.h"
#include "TerminalPane.h"
#include "TerminalTab.h"
#include "TerminalWidget.h"

#ifdef WT_HAVE_SSH
#include "ssh/ForwardingDialog.h"
#include "ssh/SftpPanel.h"
#include "ssh/SshBackend.h"
#endif

#include <QActionGroup>
#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QDesktopServices>
#include <QDir>
#include <QDockWidget>
#include <QGuiApplication>
#include <QLabel>
#include <QLineEdit>
#include <QMenuBar>
#include <QMessageBox>
#include <QStatusBar>
#include <QTabBar>
#include <QTabWidget>
#include <QUrl>

using wt::session::Action;

namespace {

bool isHeadless() {
    const QString platform = QGuiApplication::platformName();
    return platform == QLatin1String("offscreen") || platform == QLatin1String("minimal");
}

constexpr int kMaxTabTitle = 32;

} // namespace

bool MainWindow::sshSupported() {
#ifdef WT_HAVE_SSH
    return true;
#else
    return false;
#endif
}

MainWindow::MainWindow(Options options, AppContext* context, QWidget* parent)
    : QMainWindow(parent),
      m_options(std::move(options)),
      m_context(context ? context : AppContext::createEphemeral(this)),
      m_tabs(new QTabWidget(this)) {
    m_tabs->setDocumentMode(true);
    m_tabs->setTabsClosable(true);
    m_tabs->setMovable(true);
    m_tabs->setUsesScrollButtons(true);
    m_tabs->tabBar()->setAutoHide(true);
    m_tabs->tabBar()->setElideMode(Qt::ElideRight);
    setCentralWidget(m_tabs);
    statusBar(); // created up front so permanent indicators show at once
    setWindowTitle(QApplication::applicationDisplayName());

    connect(m_tabs, &QTabWidget::tabCloseRequested, this, &MainWindow::closeTab);
    connect(m_tabs, &QTabWidget::currentChanged, this, [this] {
        if (TerminalTab* tab = currentTab()) {
            m_tabs->tabBar()->setTabTextColor(m_tabs->currentIndex(), QColor());
            if (TerminalPane* pane = tab->activePane()) {
                pane->setFocus();
            }
        }
        updateTitles();
        syncFileBrowser();
        syncPaneActions();
    });
    connect(m_context, &AppContext::settingsChanged, this, &MainWindow::applySettings);
    connect(m_context, &AppContext::sessionsChanged, this, &MainWindow::rebuildSessionsMenu);

    createActions();
    createMenus();
    applyKeymap();

    // The first tab exists before start() so callers can connect to its session.
    m_firstPane = createPane();
    addTab(m_firstPane);
    if (TerminalWidget* view = m_firstPane->terminal()) {
        resize(view->sizeForGrid(m_options.session.rows, m_options.session.columns));
    }
}

MainWindow::~MainWindow() {
    m_closing = true;
    // Panes (and their sessions) go before an owned context, which they may still reference.
    delete m_fileDock;
    delete m_tabs;
}

bool MainWindow::start() {
    if (!m_firstPane) {
        return false;
    }
    if (!m_options.profileId.isEmpty()) {
        const auto* profile = m_context->sessions().find(m_options.profileId.toStdString());
        if (!profile) {
            m_errorString = tr("No saved session with id \"%1\"").arg(m_options.profileId);
            return false;
        }
        return startPaneProfile(m_firstPane, *profile);
    }
    return startPaneLocal(m_firstPane, m_options.session);
}

int MainWindow::tabCount() const {
    return m_tabs->count();
}

TerminalTab* MainWindow::tabAt(int index) const {
    return qobject_cast<TerminalTab*>(m_tabs->widget(index));
}

TerminalTab* MainWindow::currentTab() const {
    return qobject_cast<TerminalTab*>(m_tabs->currentWidget());
}

TerminalPane* MainWindow::activePane() const {
    TerminalTab* tab = currentTab();
    return tab ? tab->activePane() : nullptr;
}

TerminalSession* MainWindow::session() const {
    TerminalPane* pane = activePane();
    return pane ? pane->session() : nullptr;
}

TerminalWidget* MainWindow::terminalWidget() const {
    TerminalPane* pane = activePane();
    return pane ? pane->terminal() : nullptr;
}

// ---------------------------------------------------------------------------------------------
// Tabs and panes

TerminalPane* MainWindow::createPane() {
    auto* pane = new TerminalPane(m_context);
    pane->terminal()->setBuiltinShortcutsEnabled(false); // the window's actions handle them
    connect(pane, &TerminalPane::sessionFinished, this, &MainWindow::onPaneFinished);
    connect(pane, &TerminalPane::bellRang, this, &MainWindow::onBell);
    connect(pane, &TerminalPane::titleChanged, this, &MainWindow::updateTitles);
    connect(pane, &TerminalPane::triggerNotification, this, &MainWindow::onTriggerNotification);
    connect(pane, &TerminalPane::activated, this, &MainWindow::syncPaneActions);
    connect(pane->session(), &TerminalSession::loggingChanged, this, &MainWindow::syncPaneActions);
    QPointer<TerminalPane> guarded(pane);
    connect(pane->session(), &TerminalSession::inputSent, this, [this, guarded](const QByteArray& bytes) {
        if (guarded) {
            broadcastFrom(guarded, bytes);
        }
    });
    connect(pane->terminal(), &TerminalWidget::contextMenuAboutToShow, this, [this](QMenu* menu) {
        menu->addSeparator();
        menu->addAction(action(Action::SplitRight));
        menu->addAction(action(Action::SplitDown));
        menu->addAction(action(Action::ClosePane));
        menu->addSeparator();
        menu->addAction(action(Action::Find));
    });
    return pane;
}

TerminalTab* MainWindow::addTab(TerminalPane* pane) {
    auto* tab = new TerminalTab(pane);
    connect(tab, &TerminalTab::titleChanged, this, &MainWindow::updateTitles);
    connect(tab, &TerminalTab::activePaneChanged, this, &MainWindow::updateTitles);
    connect(tab, &TerminalTab::empty, this, [this, tab] {
        const int index = m_tabs->indexOf(tab);
        if (index >= 0) {
            m_tabs->removeTab(index);
        }
        tab->deleteLater();
        if (m_tabs->count() == 0 && !m_closing) {
            close();
        }
    });
    const int index = m_tabs->addTab(tab, pane->title());
    m_tabs->setCurrentIndex(index);
    pane->setFocus();
    updateTitles();
    return tab;
}

bool MainWindow::startPaneLocal(TerminalPane* pane, const TerminalSession::Options& options) {
    if (!pane->startLocal(options)) {
        m_errorString = pane->errorString();
        return false;
    }
    updateTitles();
    onPaneStarted(pane);
    return true;
}

void MainWindow::onPaneStarted(TerminalPane* pane) {
    if (m_context->settings().logging.autoStart && !pane->isLogging()) {
        QString error;
        if (!pane->startLogging(&error)) {
            reportError(tr("Session log: %1").arg(error));
        }
    }
    syncPaneActions();
}

QList<TerminalPane*> MainWindow::allPanes() const {
    QList<TerminalPane*> panes;
    for (int i = 0; i < m_tabs->count(); ++i) {
        if (TerminalTab* tab = tabAt(i)) {
            panes += tab->panes();
        }
    }
    return panes;
}

void MainWindow::setSessionLogging(bool enabled) {
    TerminalPane* pane = activePane();
    if (!pane) {
        return;
    }
    if (!enabled) {
        const QString path = pane->session()->logPath();
        pane->stopLogging();
        statusBar()->showMessage(tr("Session log saved to %1").arg(QDir::toNativeSeparators(path)), 8000);
    } else if (!pane->isLogging()) {
        QString error;
        if (pane->startLogging(&error)) {
            statusBar()->showMessage(
                tr("Logging to %1").arg(QDir::toNativeSeparators(pane->session()->logPath())), 8000);
        } else {
            reportError(tr("Session log: %1").arg(error));
        }
    }
    syncPaneActions();
}

void MainWindow::syncPaneActions() {
    TerminalPane* pane = activePane();
    QAction* log = action(Action::ToggleSessionLog);
    const QSignalBlocker blocker(log);
    log->setChecked(pane && pane->isLogging());
    log->setEnabled(pane != nullptr);
}

void MainWindow::setBroadcast(Broadcast mode) {
    m_broadcast = mode;
    {
        const QSignalBlocker tab(action(Action::BroadcastToTab));
        const QSignalBlocker all(action(Action::BroadcastToAll));
        action(Action::BroadcastToTab)->setChecked(mode == Broadcast::Tab);
        action(Action::BroadcastToAll)->setChecked(mode == Broadcast::All);
    }
    if (!m_broadcastLabel) {
        m_broadcastLabel = new QLabel(this);
        m_broadcastLabel->setObjectName(QStringLiteral("broadcastIndicator"));
        m_broadcastLabel->setStyleSheet(QStringLiteral("QLabel { color: #e5c07b; font-weight: bold; }"));
        statusBar()->addPermanentWidget(m_broadcastLabel);
    }
    m_broadcastLabel->setText(mode == Broadcast::Tab   ? tr("Broadcasting to the tab")
                              : mode == Broadcast::All ? tr("Broadcasting to all tabs")
                                                       : QString());
    m_broadcastLabel->setVisible(mode != Broadcast::Off);
}

void MainWindow::broadcastFrom(TerminalPane* source, const QByteArray& bytes) {
    if (m_broadcast == Broadcast::Off) {
        return;
    }
    QList<TerminalPane*> targets;
    if (m_broadcast == Broadcast::All) {
        targets = allPanes();
    } else {
        for (int i = 0; i < m_tabs->count(); ++i) {
            if (TerminalTab* tab = tabAt(i); tab && tab->panes().contains(source)) {
                targets = tab->panes();
                break;
            }
        }
    }
    const std::string_view input(bytes.constData(), static_cast<std::size_t>(bytes.size()));
    for (TerminalPane* target : targets) {
        if (target != source && target->session()->isRunning()) {
            target->session()->sendBytes(input); // not sendInput(): no echo back into the broadcast
        }
    }
}

void MainWindow::onTriggerNotification(TerminalPane* pane, const QString& title, const QString& message) {
    statusBar()->showMessage(title.isEmpty() ? message : QStringLiteral("%1: %2").arg(title, message), 10000);
    for (int i = 0; i < m_tabs->count(); ++i) {
        if (TerminalTab* tab = tabAt(i); tab && tab->panes().contains(pane) && i != m_tabs->currentIndex()) {
            m_tabs->tabBar()->setTabTextColor(i, QColor(0x61, 0xAF, 0xEF)); // trigger marker
        }
    }
    if (!isActiveWindow()) {
        QApplication::alert(this);
    }
}

bool MainWindow::startPaneProfile(TerminalPane* pane, const wt::session::SessionProfile& profile) {
    if (const auto problems = m_context->sessions().validate(profile); !problems.empty()) {
        m_errorString = tr("Session \"%1\" is incomplete: %2")
                            .arg(QString::fromStdString(profile.name), QString::fromStdString(problems.front()));
        return false;
    }
    if (profile.kind == wt::session::SessionKind::Local) {
        if (!pane->startProfile(profile)) {
            m_errorString = pane->errorString();
            return false;
        }
        updateTitles();
        onPaneStarted(pane);
        return true;
    }
    pane->startProfile(profile); // records the profile (title, theme)
    if (profile.kind == wt::session::SessionKind::Telnet || profile.kind == wt::session::SessionKind::Serial) {
        std::unique_ptr<SessionBackend> backend;
        if (profile.kind == wt::session::SessionKind::Telnet) {
            std::string proxyPassword;
            if (!profile.telnet.proxy.credentialId.empty()) {
                proxyPassword = m_context->credential(profile.telnet.proxy.credentialId, this).value_or("");
            }
            backend = std::make_unique<TelnetBackend>(TelnetBackend::fromProfile(profile, proxyPassword));
        } else {
            backend = std::make_unique<SerialBackend>(SerialBackend::fromProfile(profile));
        }
        if (!pane->session()->start(std::move(backend))) {
            m_errorString = pane->session()->errorString();
            return false;
        }
        updateTitles();
        onPaneStarted(pane);
        return true;
    }
#ifdef WT_HAVE_SSH
    SshPlan plan;
    QString error;
    if (!SshPlan::fromProfile(profile, m_context, this, plan, &error)) {
        m_errorString = error;
        return false;
    }
    auto backend = std::make_unique<SshBackend>(std::move(plan), profile.ssh.reconnect, this);
    connect(backend.get(), &SshBackend::stateChanged, this, [this](SshBackend::State state) {
        if (state == SshBackend::State::Connected) {
            syncFileBrowser();
        }
    });
    if (!pane->session()->start(std::move(backend))) {
        m_errorString = pane->session()->errorString();
        return false;
    }
    updateTitles();
    onPaneStarted(pane);
    return true;
#else
    m_errorString = tr("This build has no SSH support (libssh2 was not found)");
    return false;
#endif
}

TerminalPane* MainWindow::newTab() {
    TerminalPane* pane = createPane();
    addTab(pane);
    TerminalSession::Options options;
    options.workingDirectory = QDir::homePath();
#ifdef Q_OS_MACOS
    options.loginShell = true;
#endif
    if (!startPaneLocal(pane, options)) {
        reportError(m_errorString);
    }
    return pane;
}

TerminalPane* MainWindow::openProfile(const wt::session::SessionProfile& profile) {
    TerminalPane* pane = createPane();
    addTab(pane);
    if (!startPaneProfile(pane, profile)) {
        reportError(m_errorString);
        pane->session()->showNotice(m_errorString);
    }
    return pane;
}

TerminalPane* MainWindow::splitActive(Qt::Orientation orientation) {
    TerminalTab* tab = currentTab();
    TerminalPane* current = activePane();
    if (!tab || !current) {
        return nullptr;
    }
    TerminalPane* pane = createPane();
    tab->split(current, orientation, pane);
    bool ok = false;
    const auto* profile = current->profile();
    if (profile && (profile->kind == wt::session::SessionKind::Ssh || profile->kind == wt::session::SessionKind::Telnet)) {
        ok = startPaneProfile(pane, *profile); // a second connection to the same host
    } else {
        TerminalSession::Options options;
        // Start where the current shell is (OSC 7), like most terminals do.
        const std::string& cwd = current->session()->terminal().workingDirectory();
        options.workingDirectory = cwd.empty() ? QDir::homePath() : QString::fromStdString(cwd);
        ok = startPaneLocal(pane, options);
    }
    if (!ok) {
        reportError(m_errorString);
        pane->session()->showNotice(m_errorString);
    }
    return pane;
}

void MainWindow::closeActivePane() {
    TerminalTab* tab = currentTab();
    TerminalPane* pane = activePane();
    if (tab && pane) {
        tab->closePane(pane);
    }
}

void MainWindow::closeTab(int index) {
    TerminalTab* tab = tabAt(index);
    if (!tab) {
        return;
    }
    m_tabs->removeTab(index);
    tab->deleteLater();
    if (m_tabs->count() == 0 && !m_closing) {
        close();
    }
}

void MainWindow::onPaneFinished(TerminalPane* pane, int exitCode) {
    if (pane->isRemote()) {
        pane->session()->showNotice(tr("[Session closed (exit code %1). Use Session ▸ Reconnect to connect again.]")
                                        .arg(exitCode));
        statusBar()->showMessage(tr("%1: session closed").arg(pane->title()), 5000);
        return;
    }
    if (m_options.holdOnExit) {
        statusBar()->showMessage(tr("Process exited with code %1").arg(exitCode));
        pane->terminal()->viewport()->update();
        return;
    }
    for (int i = 0; i < m_tabs->count(); ++i) {
        if (TerminalTab* tab = tabAt(i); tab && tab->panes().contains(pane)) {
            tab->closePane(pane);
            return;
        }
    }
}

void MainWindow::onBell(TerminalPane* pane) {
    for (int i = 0; i < m_tabs->count(); ++i) {
        if (TerminalTab* tab = tabAt(i); tab && tab->panes().contains(pane) && i != m_tabs->currentIndex()) {
            m_tabs->tabBar()->setTabTextColor(i, QColor(0xE0, 0x6C, 0x75)); // attention marker
        }
    }
    if (!isActiveWindow()) {
        QApplication::alert(this);
    }
}

void MainWindow::reconnectActive() {
    if (TerminalPane* pane = activePane(); pane && pane->isRemote()) {
        if (pane->session()->backend() && pane->session()->backend()->canReconnect()) {
            pane->session()->reconnect();
        } else if (pane->profile()) {
            // The backend could not be created the first time: try again from the profile.
            const wt::session::SessionProfile profile = *pane->profile();
            TerminalTab* tab = currentTab();
            TerminalPane* replacement = createPane();
            tab->split(pane, Qt::Horizontal, replacement);
            tab->closePane(pane);
            if (!startPaneProfile(replacement, profile)) {
                replacement->session()->showNotice(m_errorString);
            }
        }
    }
}

void MainWindow::updateTitles() {
    for (int i = 0; i < m_tabs->count(); ++i) {
        if (TerminalTab* tab = tabAt(i)) {
            QString title = tab->title();
            m_tabs->setTabToolTip(i, title);
            if (title.size() > kMaxTabTitle) {
                title = title.left(kMaxTabTitle - 1) + QChar(0x2026);
            }
            if (tab->paneCount() > 1) {
                title += QStringLiteral(" [%1]").arg(tab->paneCount());
            }
            m_tabs->setTabText(i, title);
        }
    }
    const TerminalTab* tab = currentTab();
    const QString title = tab ? tab->title() : QString();
    setWindowTitle(title.isEmpty() ? QApplication::applicationDisplayName()
                                   : title + QStringLiteral(" — ") + QApplication::applicationDisplayName());
}

void MainWindow::reportError(const QString& message) {
    if (message.isEmpty()) {
        return;
    }
    statusBar()->showMessage(message, 10000);
}

void MainWindow::closeEvent(QCloseEvent* event) {
    int running = 0;
    for (int i = 0; i < m_tabs->count(); ++i) {
        if (TerminalTab* tab = tabAt(i)) {
            for (TerminalPane* pane : tab->panes()) {
                running += pane->session()->isRunning() ? 1 : 0;
            }
        }
    }
    if (running > 0 && m_context->settings().confirmCloseRunning && !isHeadless()) {
        const auto answer = QMessageBox::question(
            this, QApplication::applicationDisplayName(),
            tr("%n session(s) are still running. Close the window anyway?", nullptr, running));
        if (answer != QMessageBox::Yes) {
            event->ignore();
            return;
        }
    }
    m_closing = true;
    event->accept();
}

// ---------------------------------------------------------------------------------------------
// Actions, menus and settings

void MainWindow::createActions() {
    for (const auto& info : wt::session::allActions()) {
        auto* qaction = new QAction(tr(info.title.data()), this);
        qaction->setShortcutContext(Qt::WindowShortcut);
        const Action id = info.action;
        connect(qaction, &QAction::triggered, this, [this, id] { triggerAction(id); });
        addAction(qaction); // shortcuts work even when the menu bar is hidden
        m_actions.insert(static_cast<int>(id), qaction);
    }
    QAction* files = action(Action::ToggleFileBrowser);
    files->setCheckable(true);
    files->setEnabled(sshSupported());
    connect(files, &QAction::toggled, this, &MainWindow::setFileBrowserVisible);
    for (const Action checkable : {Action::ToggleSessionLog, Action::BroadcastToTab, Action::BroadcastToAll}) {
        action(checkable)->setCheckable(true);
    }
}

void MainWindow::triggerAction(Action id) {
    TerminalPane* pane = activePane();
    TerminalWidget* view = pane ? pane->terminal() : nullptr;
    // Text fields (find bar, dialogs) keep their own clipboard behaviour.
    auto* edit = qobject_cast<QLineEdit*>(QApplication::focusWidget());
    switch (id) {
    case Action::NewTab: newTab(); break;
    case Action::CloseTab: closeTab(m_tabs->currentIndex()); break;
    case Action::NextTab:
        if (m_tabs->count() > 1) {
            m_tabs->setCurrentIndex((m_tabs->currentIndex() + 1) % m_tabs->count());
        }
        break;
    case Action::PreviousTab:
        if (m_tabs->count() > 1) {
            m_tabs->setCurrentIndex((m_tabs->currentIndex() + m_tabs->count() - 1) % m_tabs->count());
        }
        break;
    case Action::SplitRight: splitActive(Qt::Horizontal); break;
    case Action::SplitDown: splitActive(Qt::Vertical); break;
    case Action::ClosePane: closeActivePane(); break;
    case Action::FocusNextPane:
    case Action::FocusPreviousPane:
        if (TerminalTab* tab = currentTab()) {
            tab->focusNextPane(id == Action::FocusNextPane);
        }
        break;
    case Action::Find:
        if (pane) {
            pane->showSearch();
        }
        break;
    case Action::FindNext:
        if (pane) {
            pane->findNext();
        }
        break;
    case Action::FindPrevious:
        if (pane) {
            pane->findPrevious();
        }
        break;
    case Action::Copy:
        if (edit) {
            edit->copy();
        } else if (view) {
            view->copySelection();
        }
        break;
    case Action::Paste:
        if (edit) {
            edit->paste();
        } else if (view) {
            view->paste(QApplication::clipboard()->text());
        }
        break;
    case Action::SelectAll:
        if (edit) {
            edit->selectAll();
        } else if (view) {
            view->selectAll();
        }
        break;
    case Action::ClearScrollback:
        if (view) {
            view->clearScrollback();
        }
        break;
    case Action::ZoomIn:
        if (view) {
            view->zoomIn();
        }
        break;
    case Action::ZoomOut:
        if (view) {
            view->zoomOut();
        }
        break;
    case Action::ZoomReset:
        if (view) {
            view->resetZoom();
        }
        break;
    case Action::NewSshSession: newSshSession(); break;
    case Action::NewTelnetSession: newSession(wt::session::SessionKind::Telnet); break;
    case Action::NewSerialSession: newSession(wt::session::SessionKind::Serial); break;
    case Action::SendBreak: sendBreak(); break;
    case Action::PortForwarding: showPortForwarding(); break;
    case Action::ToggleSessionLog: setSessionLogging(action(id)->isChecked()); break;
    case Action::BroadcastToTab:
        setBroadcast(action(id)->isChecked() ? Broadcast::Tab : Broadcast::Off);
        break;
    case Action::BroadcastToAll:
        setBroadcast(action(id)->isChecked() ? Broadcast::All : Broadcast::Off);
        break;
    case Action::SessionManager: showSessionManager(); break;
    case Action::Settings: showSettings(); break;
    case Action::ToggleFileBrowser: break; // a checkable action: see setFileBrowserVisible()
    case Action::Reconnect: reconnectActive(); break;
    case Action::Count_: break;
    }
}

void MainWindow::createMenus() {
    QMenu* file = menuBar()->addMenu(tr("&File"));
    file->addAction(action(Action::NewTab));
    file->addAction(action(Action::NewSshSession));
    file->addAction(action(Action::NewTelnetSession));
    file->addAction(action(Action::NewSerialSession));
    file->addAction(action(Action::SessionManager));
    file->addSeparator();
    file->addAction(action(Action::ClosePane));
    file->addAction(action(Action::CloseTab));
    file->addSeparator();
    QAction* settings = action(Action::Settings);
    settings->setMenuRole(QAction::PreferencesRole);
    file->addAction(settings);
    QAction* quit = file->addAction(tr("&Quit"), QKeySequence::Quit, qApp, &QApplication::closeAllWindows);
    quit->setMenuRole(QAction::QuitRole);

    QMenu* edit = menuBar()->addMenu(tr("&Edit"));
    edit->addAction(action(Action::Copy));
    edit->addAction(action(Action::Paste));
    edit->addAction(action(Action::SelectAll));
    edit->addSeparator();
    edit->addAction(action(Action::Find));
    edit->addAction(action(Action::FindNext));
    edit->addAction(action(Action::FindPrevious));
    edit->addSeparator();
    edit->addAction(action(Action::ClearScrollback));

    QMenu* view = menuBar()->addMenu(tr("&View"));
    view->addAction(action(Action::SplitRight));
    view->addAction(action(Action::SplitDown));
    view->addAction(action(Action::FocusNextPane));
    view->addAction(action(Action::FocusPreviousPane));
    view->addSeparator();
    view->addAction(action(Action::NextTab));
    view->addAction(action(Action::PreviousTab));
    view->addSeparator();
    view->addAction(action(Action::ZoomIn));
    view->addAction(action(Action::ZoomOut));
    view->addAction(action(Action::ZoomReset));
    view->addSeparator();
    m_themeMenu = view->addMenu(tr("&Theme"));
    rebuildThemeMenu();

    QMenu* session = menuBar()->addMenu(tr("&Session"));
    session->addAction(action(Action::Reconnect));
    session->addAction(action(Action::SendBreak));
    session->addAction(action(Action::PortForwarding));
    session->addAction(action(Action::ToggleFileBrowser));
    session->addSeparator();
    session->addAction(action(Action::ToggleSessionLog));
    session->addAction(tr("Open Logs Folder"), this, [this] {
        const QString directory = m_context->logDirectory();
        QDir().mkpath(directory);
        QDesktopServices::openUrl(QUrl::fromLocalFile(directory));
    });
    session->addAction(action(Action::BroadcastToTab));
    session->addAction(action(Action::BroadcastToAll));
    session->addSeparator();
    m_sessionsMenu = session->addMenu(tr("&Open Saved Session"));
    rebuildSessionsMenu();

    QMenu* help = menuBar()->addMenu(tr("&Help"));
    QAction* about = help->addAction(tr("&About %1").arg(QApplication::applicationDisplayName()), this, [this] {
        QMessageBox::about(this, QApplication::applicationDisplayName(),
                           tr("<b>%1</b> %2<br>A cross-platform terminal and SSH client.")
                               .arg(QApplication::applicationDisplayName(), QApplication::applicationVersion()));
    });
    about->setMenuRole(QAction::AboutRole);
}

void MainWindow::applyKeymap() {
    const wt::session::Keymap& keymap = m_context->settings().keymap;
    for (const auto& info : wt::session::allActions()) {
        QList<QKeySequence> shortcuts;
        for (const std::string& sequence : keymap.sequences(info.action)) {
            const QKeySequence key = QKeySequence::fromString(QString::fromStdString(sequence), QKeySequence::PortableText);
            if (!key.isEmpty()) {
                shortcuts << key;
            }
        }
        action(info.action)->setShortcuts(shortcuts);
    }
}

void MainWindow::applySettings() {
    applyKeymap();
    rebuildThemeMenu();
    for (int i = 0; i < m_tabs->count(); ++i) {
        if (TerminalTab* tab = tabAt(i)) {
            for (TerminalPane* pane : tab->panes()) {
                pane->applySettings();
            }
        }
    }
}

void MainWindow::rebuildThemeMenu() {
    if (!m_themeMenu) {
        return;
    }
    m_themeMenu->clear();
    auto* group = new QActionGroup(m_themeMenu);
    const auto& settings = m_context->settings();
    auto addTheme = [&](const wt::session::Theme& theme) {
        QAction* item = m_themeMenu->addAction(QString::fromStdString(theme.name));
        item->setCheckable(true);
        item->setChecked(theme.name == settings.terminal.theme);
        group->addAction(item);
        const std::string name = theme.name;
        connect(item, &QAction::triggered, this, [this, name] {
            wt::session::Settings updated = m_context->settings();
            updated.terminal.theme = name;
            m_context->setSettings(updated);
        });
    };
    for (const auto& theme : wt::session::builtinThemes()) {
        addTheme(theme);
    }
    if (!settings.customThemes.empty()) {
        m_themeMenu->addSeparator();
        for (const auto& theme : settings.customThemes) {
            addTheme(theme);
        }
    }
}

void MainWindow::rebuildSessionsMenu() {
    if (!m_sessionsMenu) {
        return;
    }
    m_sessionsMenu->clear();
    const auto& profiles = m_context->sessions().profiles();
    if (profiles.empty()) {
        m_sessionsMenu->addAction(tr("(no saved sessions)"))->setEnabled(false);
        return;
    }
    QHash<QString, QMenu*> groups;
    for (const auto& profile : profiles) {
        QMenu* menu = m_sessionsMenu;
        if (!profile.group.empty()) {
            const QString group = QString::fromStdString(profile.group);
            if (!groups.contains(group)) {
                groups.insert(group, m_sessionsMenu->addMenu(group));
            }
            menu = groups.value(group);
        }
        const std::string id = profile.id;
        menu->addAction(QString::fromStdString(profile.name), this, [this, id] {
            if (const auto* p = m_context->sessions().find(id)) {
                openProfile(*p);
            }
        });
    }
}

void MainWindow::showSettings() {
    SettingsDialog dialog(m_context->settings(), this);
    if (dialog.exec() == QDialog::Accepted) {
        if (!m_context->setSettings(dialog.settings())) {
            reportError(tr("Could not save settings: %1").arg(m_context->lastError()));
        }
    }
}

void MainWindow::showSessionManager() {
    SessionManagerDialog dialog(m_context, this);
    connect(&dialog, &SessionManagerDialog::openRequested, this,
            [this](const wt::session::SessionProfile& profile) { openProfile(profile); });
    dialog.exec();
}

void MainWindow::newSshSession() {
    newSession(wt::session::SessionKind::Ssh);
}

void MainWindow::sendBreak() {
    TerminalPane* pane = activePane();
    SessionBackend* backend = pane ? pane->session()->backend() : nullptr;
    if (auto* telnet = qobject_cast<TelnetBackend*>(backend)) {
        telnet->sendCommand(wt::net::TelnetCommand::Break);
    } else if (auto* serial = qobject_cast<SerialBackend*>(backend)) {
        serial->sendBreak();
    } else {
        statusBar()->showMessage(tr("Send Break works in Telnet and serial sessions"), 5000);
        return;
    }
    statusBar()->showMessage(tr("Break sent"), 2000);
}

void MainWindow::showPortForwarding() {
#ifdef WT_HAVE_SSH
    TerminalPane* pane = activePane();
    auto* backend = pane ? qobject_cast<SshBackend*>(pane->session()->backend()) : nullptr;
    if (!backend) {
        statusBar()->showMessage(tr("Port forwarding works in SSH sessions"), 5000);
        return;
    }
    ForwardingDialog dialog(backend, this);
    dialog.exec();
#else
    statusBar()->showMessage(tr("This build has no SSH support (libssh2 was not found)"), 5000);
#endif
}

void MainWindow::newSession(wt::session::SessionKind kind) {
    wt::session::SessionProfile profile;
    profile.kind = kind;
    SessionEditDialog dialog(m_context, profile, this);
    dialog.setWindowTitle(SessionEditDialog::titleForNew(kind));
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    wt::session::SessionProfile edited = dialog.profile();
    if (dialog.saveRequested()) {
        edited.id = m_context->sessions().upsert(edited);
        if (!m_context->saveSessions()) {
            reportError(tr("Could not save sessions: %1").arg(m_context->lastError()));
        }
    } else if (edited.id.empty()) {
        edited.id = wt::session::SessionStore::generateId();
    }
    openProfile(edited);
}

// ---------------------------------------------------------------------------------------------
// SFTP file browser

void MainWindow::setFileBrowserVisible(bool visible) {
#ifdef WT_HAVE_SSH
    if (visible && !m_fileDock) {
        m_fileDock = new QDockWidget(tr("Files"), this);
        m_fileDock->setObjectName(QStringLiteral("FileBrowserDock"));
        m_fileBrowser = new SftpPanel(m_fileDock);
        m_fileDock->setWidget(m_fileBrowser);
        addDockWidget(Qt::RightDockWidgetArea, m_fileDock);
        connect(m_fileDock, &QDockWidget::visibilityChanged, this, [this](bool shown) {
            if (!shown && !m_closing && !isMinimized()) {
                const QSignalBlocker blocker(action(Action::ToggleFileBrowser));
                action(Action::ToggleFileBrowser)->setChecked(false);
            }
        });
        connect(m_fileBrowser, &SftpPanel::operationFinished, this, [this](bool ok, const QString& message) {
            if (!ok && !message.isEmpty()) {
                statusBar()->showMessage(tr("File browser: %1").arg(message), 8000);
            }
        });
    }
    if (m_fileDock) {
        m_fileDock->setVisible(visible);
    }
    {
        const QSignalBlocker blocker(action(Action::ToggleFileBrowser));
        action(Action::ToggleFileBrowser)->setChecked(visible);
    }
    if (visible) {
        syncFileBrowser();
    }
#else
    Q_UNUSED(visible);
#endif
}

void MainWindow::syncFileBrowser() {
#ifdef WT_HAVE_SSH
    if (!m_fileDock || !m_fileDock->isVisible() || !m_fileBrowser) {
        return;
    }
    TerminalPane* pane = activePane();
    auto* backend = pane ? qobject_cast<SshBackend*>(pane->session()->backend()) : nullptr;
    if (!backend || backend->state() != SshBackend::State::Connected) {
        if (!backend) {
            statusBar()->showMessage(tr("The file browser shows the active SSH session"), 5000);
        }
        return;
    }
    const SshPlan plan = backend->currentPlan();
    if (m_fileBrowser->target() != plan.description || !m_fileBrowser->isConnected()) {
        if (!m_fileBrowser->isBusy() || m_fileBrowser->target() != plan.description) {
            m_fileBrowser->connectTo(plan, this);
        }
    }
#endif
}
