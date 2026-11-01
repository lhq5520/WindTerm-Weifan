#pragma once

#include "TerminalSession.h"

#include "wt/session/SessionProfile.h"
#include "wt/session/Workspace.h"

#include <QPointer>
#include <QWidget>

class AppContext;
class SearchBar;
class TerminalWidget;

// One terminal in a tab: the view, its find bar and the session it shows.
class TerminalPane : public QWidget {
    Q_OBJECT

public:
    explicit TerminalPane(AppContext* context, QWidget* parent = nullptr);
    ~TerminalPane() override;

    TerminalSession* session() const { return m_session; }
    TerminalWidget* terminal() const { return m_terminal; }
    SearchBar* searchBar() const { return m_searchBar; }

    // Starts a local process.
    bool startLocal(const TerminalSession::Options& options);
    // Starts a saved session (local or SSH).
    bool startProfile(const wt::session::SessionProfile& profile);
    QString errorString() const { return m_session->errorString(); }

    // Saved session this pane was opened from (empty for ad-hoc local shells).
    const wt::session::SessionProfile* profile() const { return m_profile ? &*m_profile : nullptr; }
    bool isRemote() const;

    // Window/tab title: the terminal's title, else the profile name, else the program.
    QString title() const;

    void showSearch();
    void findNext();
    void findPrevious();

    // Re-applies the application settings (theme, font, behaviour, triggers, completion).
    void applySettings();

    // Session log to a new file in the configured log directory.
    bool startLogging(QString* error = nullptr);
    void stopLogging();
    bool isLogging() const { return m_session->isLogging(); }
    // Where commands typed here are remembered: the profile id, or "local".
    QString historyContext() const;
    // Host-ish name for log file names ("db.example.com", "ttyUSB0", "local").
    QString hostName() const;
    // What it takes to open this pane again (session restore).
    wt::session::PaneState paneState() const;

Q_SIGNALS:
    void titleChanged();
    void activated(TerminalPane* pane);
    void sessionFinished(TerminalPane* pane, int exitCode);
    void bellRang(TerminalPane* pane);
    // A notify trigger matched.
    void triggerNotification(TerminalPane* pane, const QString& title, const QString& message);

private:
    void wireSession();

    AppContext* m_context; // outlives the pane (may be null in tests)
    TerminalSession* m_session;
    TerminalWidget* m_terminal;
    SearchBar* m_searchBar;
    std::optional<wt::session::SessionProfile> m_profile;
    TerminalSession::Options m_localOptions; // how the local process was started
    QString m_fallbackTitle;
};
