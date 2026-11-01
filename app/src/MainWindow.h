#pragma once

#include "TerminalSession.h"

#include "wt/session/Keymap.h"
#include "wt/session/SessionProfile.h"

#include <QHash>
#include <QMainWindow>
#include <QPointer>

#include <functional>
#include <memory>

class AppContext;
class QDockWidget;
class QMenu;
class SftpPanel;
class QTabWidget;
class TerminalPane;
class TerminalTab;
class TerminalWidget;

// Top-level window: tabs of (split) terminal panes, menus and configurable shortcuts.
class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    struct Options {
        TerminalSession::Options session; // the first tab's local shell
        bool holdOnExit = false;          // keep panes open after their program exits
        QString profileId;                // open this saved session instead of a local shell
    };

    // `context` may be null: the window then uses defaults and saves nothing.
    explicit MainWindow(Options options, AppContext* context = nullptr, QWidget* parent = nullptr);
    ~MainWindow() override;

    // Starts the first tab; on failure errorString() explains why.
    bool start();
    QString errorString() const { return m_errorString; }

    AppContext* context() const { return m_context; }
    QTabWidget* tabs() const { return m_tabs; }
    int tabCount() const;
    TerminalTab* currentTab() const;
    TerminalTab* tabAt(int index) const;
    TerminalPane* activePane() const;
    // Active pane's session / view (the first tab's before start()).
    TerminalSession* session() const;
    TerminalWidget* terminalWidget() const;
    QAction* action(wt::session::Action action) const { return m_actions.value(static_cast<int>(action)); }
    // The SFTP file browser (created on first use; null in builds without SSH).
    SftpPanel* fileBrowser() const { return m_fileBrowser; }
    static bool sshSupported();

public Q_SLOTS:
    TerminalPane* newTab();
    // Opens a saved session in a new tab (or splits the active pane when `split` is set).
    TerminalPane* openProfile(const wt::session::SessionProfile& profile);
    TerminalPane* splitActive(Qt::Orientation orientation);
    void closeActivePane();
    void closeTab(int index);
    void showSettings();
    void showSessionManager();
    void newSshSession();
    // Asks for a new session of `kind` (SSH, Telnet, serial, local) and opens it.
    void newSession(wt::session::SessionKind kind);
    void reconnectActive();
    // Telnet BREAK / serial break condition on the active pane.
    void sendBreak();
    // Lists/edits the port forwards of the active SSH session.
    void showPortForwarding();
    // Shows/hides the SFTP browser for the active SSH pane.
    void setFileBrowserVisible(bool visible);

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    TerminalPane* createPane();
    TerminalTab* addTab(TerminalPane* pane);
    bool startPaneLocal(TerminalPane* pane, const TerminalSession::Options& options);
    bool startPaneProfile(TerminalPane* pane, const wt::session::SessionProfile& profile);
    void createActions();
    void createMenus();
    void applyKeymap();
    void applySettings();
    void triggerAction(wt::session::Action action);
    void updateTitles();
    void onPaneFinished(TerminalPane* pane, int exitCode);
    void onBell(TerminalPane* pane);
    void rebuildSessionsMenu();
    void rebuildThemeMenu();
    void reportError(const QString& message);
    void syncFileBrowser();

    Options m_options;
    AppContext* m_context; // outlives the window (or is owned by it, see the constructor)
    QTabWidget* m_tabs;
    TerminalPane* m_firstPane = nullptr;
    QHash<int, QAction*> m_actions;
    QMenu* m_sessionsMenu = nullptr;
    QMenu* m_themeMenu = nullptr;
    QString m_errorString;
    QDockWidget* m_fileDock = nullptr;
    SftpPanel* m_fileBrowser = nullptr;
    bool m_closing = false;
};
