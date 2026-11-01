#pragma once

#include "Triggers.h"

#include "wt/session/CommandHistory.h"
#include "wt/session/Settings.h"
#include "wt/session/Theme.h"
#include "wt/terminal/Cell.h"
#include "wt/terminal/KeyEncoder.h"
#include "wt/terminal/MouseEncoder.h"
#include "wt/terminal/Search.h"
#include "wt/terminal/Selection.h"

#include <QAbstractScrollArea>
#include <QColor>
#include <QElapsedTimer>
#include <QFont>
#include <QPointer>
#include <QTimer>
#include <QUrl>

#include <functional>
#include <optional>
#include <vector>

class QListWidget;
class QMenu;
class TerminalSession;
namespace wt::terminal {
struct Line;
}

// User-configurable look and behaviour of a terminal view (see wt::session::TerminalSettings).
struct TerminalAppearance {
    wt::session::Theme theme;
    QFont font; // base font; zoom is applied on top
    wt::session::CursorStyle cursorStyle = wt::session::CursorStyle::Block;
    bool cursorBlink = false;
    bool optionAsMeta = false;
    bool copyOnSelect = false;
    bool allowClipboardWrite = false;
    wt::session::BellStyle bell = wt::session::BellStyle::Visual;

    static QFont defaultFont();
    // Settings -> appearance (resolving the theme and the font).
    static TerminalAppearance fromSettings(const wt::session::Settings& settings, const QString& themeOverride = {});
};

// Renders a TerminalSession's screen and scrollback, forwards keyboard / IME / mouse input and
// implements selection, links, search highlighting and the clipboard.
class TerminalWidget : public QAbstractScrollArea {
    Q_OBJECT

public:
    explicit TerminalWidget(QWidget* parent = nullptr);

    void setSession(TerminalSession* session);
    TerminalSession* session() const;

    void setAppearance(const TerminalAppearance& appearance);
    const TerminalAppearance& appearance() const { return m_appearance; }
    void setTerminalFont(const QFont& font);
    QFont terminalFont() const { return m_fonts[0]; }
    QSize cellSize() const { return {m_cellWidth, m_cellHeight}; }
    QSize sizeForGrid(int rows, int columns) const;
    bool isScrolledToBottom() const;
    // Resizes the session to fit the view (normally done on resize events).
    void syncGridSize() { updateGridSize(); }

    // Font zoom in steps of one point relative to the appearance font.
    void zoomIn();
    void zoomOut();
    void resetZoom();
    int zoomSteps() const { return m_zoomSteps; }

    // Copy/paste/select-all key handling inside the widget (on by default). Hosts with their own
    // configurable shortcuts turn it off.
    void setBuiltinShortcutsEnabled(bool enabled) { m_builtinShortcuts = enabled; }
    // Open activated links with QDesktopServices (on by default); linkActivated() is always emitted.
    void setOpenLinksExternally(bool enabled) { m_openLinks = enabled; }

    void paste(const QString& text);
    // Copies the selection to the clipboard; returns false if nothing is selected.
    bool copySelection();
    void selectAll();
    void clearScrollback();
    QString selectedText() const;
    // Viewport cell -> absolute grid position (see Terminal::historyStart()).
    wt::terminal::GridPoint gridPointAt(const QPoint& viewportPos) const;
    // Link (OSC 8 hyperlink or a URL in the text) under a viewport position, empty if none.
    QString linkAt(const QPoint& viewportPos) const;

    // Search results to highlight; `current` (index into matches, or -1) is emphasised.
    void setSearchResults(std::vector<wt::terminal::SearchMatch> matches, int current);
    const std::vector<wt::terminal::SearchMatch>& searchResults() const { return m_searchMatches; }
    // Scrolls so that an absolute row is visible (roughly centred).
    void scrollToRow(std::int64_t absoluteRow);

    // Highlight triggers (rules with TriggerAction::Highlight) applied when painting.
    void setHighlightRules(std::vector<CompiledTrigger> rules);
    // Matches of the highlight rules on an absolute row: (first column, last column, rule index).
    struct RuleMatch {
        int startColumn;
        int endColumn;
        int rule;
    };
    std::vector<RuleMatch> highlightMatches(std::int64_t absoluteRow) const;

    // Command completion: `provider(prefix)` returns history entries starting with what is typed
    // at the prompt; an empty provider turns completion off. Suggestions show after
    // `minimumPrefix` characters; Up/Down choose one, Right/Enter/Tab insert it, Esc closes.
    using CompletionProvider = std::function<QStringList(const QString& prefix)>;
    void setCompletionProvider(CompletionProvider provider, int minimumPrefix = 2);
    bool completionVisible() const;
    QStringList completionItems() const;
    // Inserts the rest of suggestion `row` as if it was typed.
    void acceptCompletion(int row);
    // What the user is typing at the prompt, as far as the keystrokes tell.
    QString typedLine() const { return QString::fromStdString(m_inputLine.line()); }

    QSize sizeHint() const override;
    QVariant inputMethodQuery(Qt::InputMethodQuery query) const override;

Q_SIGNALS:
    void linkActivated(const QUrl& url);
    void bellRang();
    void focusReceived();
    // Lets the host add entries (split, find, ...) to the context menu before it is shown.
    void contextMenuAboutToShow(QMenu* menu);
    // A command line the user ran at a shell prompt (for the completion history). Only lines that
    // were echoed on the screen are reported, so passwords typed at silent prompts never are.
    void commandSubmitted(const QString& command);

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void keyReleaseEvent(QKeyEvent* event) override;
    void inputMethodEvent(QInputMethodEvent* event) override;
    void focusInEvent(QFocusEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;
    void leaveEvent(QEvent* event) override;
    bool focusNextPrevChild(bool next) override;
    void scrollContentsBy(int dx, int dy) override;

private:
    struct HoverLink {
        std::int64_t row = -1;
        int startColumn = 0;
        int endColumn = 0; // inclusive
        QString url;
    };
    enum class Highlight : std::uint8_t { None, Rule, Match, CurrentMatch, Selection };
    struct RowHighlight {
        int startColumn;
        int endColumn; // inclusive
        Highlight kind;
        int rule = -1; // Highlight::Rule: index into m_highlightRules
    };

    void onOutput();
    void onBell();
    void applyFont();
    void updateGridSize();
    void updateScrollBar(bool stickToBottom);
    void scrollToBottom();
    bool handleShortcut(QKeyEvent* event);
    std::int64_t viewportTopRow() const;
    QRect rowRect(int viewportRow) const;
    void updateRow(int viewportRow);
    QColor resolveColor(const wt::terminal::Color& color, bool foreground, bool bold) const;
    void drawLine(QPainter& painter, const wt::terminal::Line& line, int y, std::int64_t absoluteRow,
                  const std::optional<wt::terminal::SelectionRange>& selection,
                  const std::vector<RowHighlight>& highlights) const;
    void drawDecorations(QPainter& painter, const wt::terminal::CellAttributes& attr, const QRect& rect,
                         const QColor& foreground) const;
    void drawCursor(QPainter& painter) const;
    std::vector<RowHighlight> searchHighlightsForRow(std::int64_t absoluteRow) const;

    // User input (keys, IME, paste): command tracking, completion, then the session.
    void sendUserInput(std::string_view bytes);
    // Bytes for a key press / repeat / release in the current keyboard modes.
    std::string encodeKeyEvent(const QKeyEvent* event, wt::terminal::KeyAction action) const;
    void updateCompletion();
    void hideCompletion();

    // Links
    std::optional<HoverLink> findLink(std::int64_t row, int column) const;
    void updateHover(const QPoint& pos, Qt::KeyboardModifiers modifiers);
    void activateLink(const QString& url);

    // Mouse helpers
    bool reportsMouse(Qt::KeyboardModifiers modifiers) const;
    bool sendMouseEvent(wt::terminal::MouseAction action, wt::terminal::MouseButton button, const QPoint& pos,
                        Qt::KeyboardModifiers modifiers);
    void updateSelectionDrag(const QPoint& pos);
    void autoScrollSelection();
    void copyToPrimarySelection();
    void finishSelection();

    TerminalSession* m_session = nullptr; // not owned; reset when the session is destroyed
    TerminalAppearance m_appearance;
    QFont m_fonts[4]; // regular, bold, italic, bold italic
    int m_cellWidth = 8;
    int m_cellHeight = 16;
    int m_ascent = 12;
    int m_zoomSteps = 0;
    QColor m_selectionBackground{0x26, 0x4F, 0x78};
    QString m_preedit;
    bool m_builtinShortcuts = true;
    bool m_openLinks = true;

    // Selection drag state
    bool m_selecting = false;
    QPoint m_pressPos;
    wt::terminal::GridPoint m_pressPoint;
    wt::terminal::SelectionMode m_dragMode = wt::terminal::SelectionMode::Character;
    QPoint m_lastDragPos;
    QTimer m_autoScrollTimer;
    QElapsedTimer m_lastDoubleClick;
    QPoint m_doubleClickPos;

    // Mouse reporting state
    std::optional<wt::terminal::MouseButton> m_reportedButton;
    QPoint m_lastReportedCell{-1, -1};
    int m_wheelAccumulator = 0;

    // Links
    std::optional<HoverLink> m_hoverLink;

    // Search highlighting
    std::vector<wt::terminal::SearchMatch> m_searchMatches;
    int m_currentMatch = -1;
    std::vector<CompiledTrigger> m_highlightRules;

    // Command line tracking and completion
    wt::session::InputLineTracker m_inputLine;
    std::optional<wt::terminal::GridPoint> m_promptEnd; // where typing started on the current line
    CompletionProvider m_completionProvider;
    int m_completionMinimum = 2;
    QListWidget* m_completion = nullptr;

    // Painting state
    int m_lastCursorRow = 0;
    bool m_cursorBlinkOn = true;
    QTimer m_blinkTimer;
    QTimer m_flashTimer; // visual bell
    // Synchronized output (?2026): repaint at the latest after this fallback fires.
    QTimer m_syncTimer;
};
