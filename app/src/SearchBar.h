#pragma once

#include "wt/terminal/Search.h"

#include <QPointer>
#include <QRegularExpression>
#include <QTimer>
#include <QWidget>

class QLabel;
class QLineEdit;
class QToolButton;
class TerminalWidget;

// Search engine backed by QRegularExpression (PCRE2), used for regex searches in the GUI.
wt::terminal::SearchMatcher qtRegexMatcher(const QRegularExpression& regex);

// Find bar shown above a terminal: incremental search over the scrollback with case, whole-word
// and regex options; highlights all matches and steps through them.
class SearchBar : public QWidget {
    Q_OBJECT

public:
    explicit SearchBar(TerminalWidget* terminal, QWidget* parent = nullptr);

    void activate(); // show, focus and select the query
    void setQuery(const QString& text);
    QString query() const;
    void setOptions(bool caseSensitive, bool wholeWord, bool regex);

    // Moves to the next (newer, downwards) / previous (older, upwards) match.
    void findNext();
    void findPrevious();
    // Re-runs the search (e.g. after new output), keeping the current match where possible.
    void refresh();

    int matchCount() const;
    int currentIndex() const { return m_current; }
    QString errorText() const { return m_error; }

Q_SIGNALS:
    void closed();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    void runSearch(bool keepPosition);
    void select(int index);
    void updateLabel();

    TerminalWidget* m_terminal; // sibling view; reset when it is destroyed
    QLineEdit* m_edit;
    QToolButton* m_case;
    QToolButton* m_word;
    QToolButton* m_regex;
    QLabel* m_label;
    QTimer m_debounce;
    QTimer m_refresh;
    int m_current = -1;
    QString m_error;
};
