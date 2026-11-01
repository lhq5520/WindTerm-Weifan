#pragma once

#include "wt/session/Workspace.h"

#include <QList>
#include <QPointer>
#include <QWidget>

class QSplitter;
class TerminalPane;

// A tab holding one or more panes arranged in nested splitters.
class TerminalTab : public QWidget {
    Q_OBJECT

public:
    explicit TerminalTab(TerminalPane* first, QWidget* parent = nullptr);

    // Panes in visual order (depth-first, left-to-right / top-to-bottom).
    QList<TerminalPane*> panes() const;
    int paneCount() const { return static_cast<int>(panes().size()); }
    TerminalPane* activePane() const;
    void setActivePane(TerminalPane* pane);

    // Places `newPane` next to `target` (right of it for Qt::Horizontal, below for Qt::Vertical),
    // splitting the space evenly, and focuses it.
    void split(TerminalPane* target, Qt::Orientation orientation, TerminalPane* newPane);
    // Removes and deletes a pane; collapses splitters left with a single child. Emits empty()
    // when the last pane goes.
    void closePane(TerminalPane* pane);
    // Moves the focus to the next/previous pane (wrapping around).
    void focusNextPane(bool forward);

    QString title() const;
    // The split layout with what every pane shows (session restore).
    wt::session::LayoutNode captureLayout() const;

Q_SIGNALS:
    void empty();
    void activePaneChanged(TerminalPane* pane);
    void titleChanged();

private:
    void adopt(TerminalPane* pane);
    void collectPanes(QWidget* widget, QList<TerminalPane*>& out) const;

    QWidget* m_root = nullptr; // a TerminalPane or a QSplitter
    QPointer<TerminalPane> m_active;
};
