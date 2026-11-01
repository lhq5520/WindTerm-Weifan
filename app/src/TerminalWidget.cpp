#include "TerminalWidget.h"

#include "TerminalSession.h"

#include "wt/core/Utf8.h"
#include "wt/terminal/KeyEncoder.h"
#include "wt/terminal/Terminal.h"

#include <QApplication>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QDesktopServices>
#include <QFontDatabase>
#include <QFontMetricsF>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QListWidget>
#include <QMenu>
#include <QPainter>
#include <QPainterPath>
#include <QRegularExpression>
#include <QScrollBar>
#include <QStyleHints>

#include <algorithm>
#include <cmath>
#include <optional>

using namespace wt::terminal;

namespace {

// On macOS Qt reports Command as ControlModifier and the physical Control key as MetaModifier.
#ifdef Q_OS_MACOS
constexpr Qt::KeyboardModifier kTerminalControl = Qt::MetaModifier;
constexpr Qt::KeyboardModifier kAppCommand = Qt::ControlModifier;
#else
constexpr Qt::KeyboardModifier kTerminalControl = Qt::ControlModifier;
#endif
// Modifier that turns a click on a link into "open it" (plain clicks select text).
constexpr Qt::KeyboardModifier kLinkModifier = Qt::ControlModifier;

// Longest a synchronized update (?2026) may hold back painting.
constexpr int kSyncTimeoutMs = 150;
constexpr int kAutoScrollIntervalMs = 50;
constexpr int kBlinkIntervalMs = 530;
constexpr int kVisualBellMs = 120;
constexpr int kMinFontPointSize = 4;

const QColor kMatchBackground(0x61, 0x5A, 0x1F);
const QColor kCurrentMatchBackground(0xC8, 0x82, 0x1E);

unsigned toTerminalModifiers(Qt::KeyboardModifiers mods) {
    unsigned result = NoModifier;
    if (mods & Qt::ShiftModifier) {
        result |= ShiftModifier;
    }
    if (mods & Qt::AltModifier) {
        result |= AltModifier;
    }
    if (mods & kTerminalControl) {
        result |= ControlModifier;
    }
    return result;
}

std::optional<Key> mapKey(int key) {
    switch (key) {
    case Qt::Key_Return:
    case Qt::Key_Enter: return Key::Enter;
    case Qt::Key_Tab:
    case Qt::Key_Backtab: return Key::Tab;
    case Qt::Key_Backspace: return Key::Backspace;
    case Qt::Key_Escape: return Key::Escape;
    case Qt::Key_Up: return Key::Up;
    case Qt::Key_Down: return Key::Down;
    case Qt::Key_Left: return Key::Left;
    case Qt::Key_Right: return Key::Right;
    case Qt::Key_Home: return Key::Home;
    case Qt::Key_End: return Key::End;
    case Qt::Key_Insert: return Key::Insert;
    case Qt::Key_Delete: return Key::Delete;
    case Qt::Key_PageUp: return Key::PageUp;
    case Qt::Key_PageDown: return Key::PageDown;
    default: break;
    }
    if (key >= Qt::Key_F1 && key <= Qt::Key_F12) {
        return static_cast<Key>(static_cast<int>(Key::F1) + (key - Qt::Key_F1));
    }
    return std::nullopt;
}

// Digits and operators on the numeric keypad (arrows also carry KeypadModifier on macOS).
std::optional<Key> mapKeypadKey(int key) {
    if (key >= Qt::Key_0 && key <= Qt::Key_9) {
        return static_cast<Key>(static_cast<int>(Key::Keypad0) + (key - Qt::Key_0));
    }
    switch (key) {
    case Qt::Key_Period:
    case Qt::Key_Comma: return Key::KeypadDecimal;
    case Qt::Key_Enter: return Key::KeypadEnter;
    case Qt::Key_Plus: return Key::KeypadPlus;
    case Qt::Key_Minus: return Key::KeypadMinus;
    case Qt::Key_Asterisk: return Key::KeypadMultiply;
    case Qt::Key_Slash: return Key::KeypadDivide;
    case Qt::Key_Equal: return Key::KeypadEqual;
    default: return std::nullopt;
    }
}

std::optional<MouseButton> mapButton(Qt::MouseButton button) {
    switch (button) {
    case Qt::LeftButton: return MouseButton::Left;
    case Qt::MiddleButton: return MouseButton::Middle;
    case Qt::RightButton: return MouseButton::Right;
    default: return std::nullopt;
    }
}

QColor fromRgb(std::uint32_t rgb) {
    return QColor((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);
}

QString cellString(const Terminal& term, const Cell& cell) {
    const std::u32string_view text = term.cellText(cell);
    return QString::fromUcs4(text.data(), static_cast<qsizetype>(text.size()));
}

CursorShape toCursorShape(wt::session::CursorStyle style) {
    switch (style) {
    case wt::session::CursorStyle::Underline: return CursorShape::Underline;
    case wt::session::CursorStyle::Bar: return CursorShape::Bar;
    case wt::session::CursorStyle::Block: break;
    }
    return CursorShape::Block;
}

bool isSafeLinkScheme(const QUrl& url) {
    static const QStringList kSchemes = {QStringLiteral("http"), QStringLiteral("https"), QStringLiteral("ftp"),
                                         QStringLiteral("file"), QStringLiteral("mailto")};
    return url.isValid() && kSchemes.contains(url.scheme().toLower());
}

} // namespace

QFont TerminalAppearance::defaultFont() {
    QFont font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
#ifdef Q_OS_MACOS
    font.setFamilies({QStringLiteral("SF Mono"), QStringLiteral("Menlo"), font.family()});
    font.setPointSize(13);
#else
    font.setPointSize(std::max(font.pointSize(), 11));
#endif
    font.setStyleHint(QFont::Monospace);
    font.setFixedPitch(true);
    return font;
}

TerminalAppearance TerminalAppearance::fromSettings(const wt::session::Settings& settings,
                                                    const QString& themeOverride) {
    TerminalAppearance appearance;
    const auto& t = settings.terminal;
    appearance.theme = settings.activeTheme();
    if (!themeOverride.isEmpty()) {
        if (auto theme = wt::session::findTheme(themeOverride.toStdString(), settings.customThemes)) {
            appearance.theme = *theme;
        }
    }
    appearance.font = defaultFont();
    if (!t.fontFamily.empty()) {
        appearance.font.setFamilies({QString::fromStdString(t.fontFamily), appearance.font.family()});
    }
    if (t.fontSize > 0) {
        appearance.font.setPointSizeF(t.fontSize);
    }
    appearance.cursorStyle = t.cursorStyle;
    appearance.cursorBlink = t.cursorBlink;
    appearance.optionAsMeta = t.optionAsMeta;
    appearance.copyOnSelect = t.copyOnSelect;
    appearance.allowClipboardWrite = t.allowClipboardWrite;
    appearance.bell = t.bell;
    return appearance;
}

TerminalWidget::TerminalWidget(QWidget* parent) : QAbstractScrollArea(parent) {
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_InputMethodEnabled, true);
    setFrameShape(QFrame::NoFrame);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    // Always shown: an on-demand scrollbar would change the column count (and SIGWINCH the
    // shell) the first time scrollback appears.
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
    viewport()->setAttribute(Qt::WA_OpaquePaintEvent, true);
    viewport()->setCursor(Qt::IBeamCursor);
    viewport()->setMouseTracking(true); // hover motion for ?1003 and links

    m_autoScrollTimer.setInterval(kAutoScrollIntervalMs);
    connect(&m_autoScrollTimer, &QTimer::timeout, this, &TerminalWidget::autoScrollSelection);
    m_syncTimer.setSingleShot(true);
    m_syncTimer.setInterval(kSyncTimeoutMs);
    connect(&m_syncTimer, &QTimer::timeout, this, [this] { viewport()->update(); });
    m_blinkTimer.setInterval(kBlinkIntervalMs);
    connect(&m_blinkTimer, &QTimer::timeout, this, [this] {
        m_cursorBlinkOn = !m_cursorBlinkOn;
        if (m_session) {
            updateRow(m_session->terminal().cursor().row);
        }
    });
    m_flashTimer.setSingleShot(true);
    m_flashTimer.setInterval(kVisualBellMs);
    connect(&m_flashTimer, &QTimer::timeout, this, [this] { viewport()->update(); });

    m_appearance.font = TerminalAppearance::defaultFont();
    setAppearance(m_appearance);
}

TerminalSession* TerminalWidget::session() const {
    return m_session;
}

void TerminalWidget::setSession(TerminalSession* session) {
    if (m_session) {
        disconnect(m_session, nullptr, this, nullptr);
    }
    m_session = session;
    m_hoverLink.reset();
    m_searchMatches.clear();
    m_currentMatch = -1;
    if (m_session) {
        connect(m_session, &QObject::destroyed, this, [this] {
            m_session = nullptr;
            viewport()->update();
        });
        connect(m_session, &TerminalSession::outputReceived, this, &TerminalWidget::onOutput);
        connect(m_session, &TerminalSession::bell, this, &TerminalWidget::onBell);
        connect(m_session, &TerminalSession::clipboardWriteRequested, this, [](char target, const QString& text) {
            QClipboard* clipboard = QApplication::clipboard();
            if (target == 'p' && clipboard->supportsSelection()) {
                clipboard->setText(text, QClipboard::Selection);
            } else {
                clipboard->setText(text, QClipboard::Clipboard);
            }
        });
        Terminal& term = m_session->terminal();
        term.setDefaultPalette(m_appearance.theme.palette);
        term.setDefaultCursorStyle(toCursorShape(m_appearance.cursorStyle), m_appearance.cursorBlink);
        term.setClipboardWriteAllowed(m_appearance.allowClipboardWrite);
        updateGridSize();
    }
    updateScrollBar(true);
    viewport()->update();
}

void TerminalWidget::setAppearance(const TerminalAppearance& appearance) {
    m_appearance = appearance;
    m_selectionBackground = fromRgb(appearance.theme.selectionBackground);
    if (m_session) {
        Terminal& term = m_session->terminal();
        term.setDefaultPalette(appearance.theme.palette);
        term.setDefaultCursorStyle(toCursorShape(appearance.cursorStyle), appearance.cursorBlink);
        term.setClipboardWriteAllowed(appearance.allowClipboardWrite);
    }
    applyFont();
}

void TerminalWidget::applyFont() {
    QFont font = m_appearance.font;
    const qreal base = font.pointSizeF() > 0 ? font.pointSizeF() : 11.0;
    font.setPointSizeF(std::max<qreal>(kMinFontPointSize, base + m_zoomSteps));
    setTerminalFont(font);
}

void TerminalWidget::zoomIn() {
    ++m_zoomSteps;
    applyFont();
}

void TerminalWidget::zoomOut() {
    const qreal base = m_appearance.font.pointSizeF() > 0 ? m_appearance.font.pointSizeF() : 11.0;
    if (base + m_zoomSteps - 1 >= kMinFontPointSize) {
        --m_zoomSteps;
        applyFont();
    }
}

void TerminalWidget::resetZoom() {
    m_zoomSteps = 0;
    applyFont();
}

void TerminalWidget::setTerminalFont(const QFont& font) {
    QFont regular = font;
    regular.setKerning(false);
    regular.setStyleStrategy(QFont::PreferAntialias);
    m_fonts[0] = regular;
    m_fonts[1] = regular;
    m_fonts[1].setBold(true);
    m_fonts[2] = regular;
    m_fonts[2].setItalic(true);
    m_fonts[3] = m_fonts[1];
    m_fonts[3].setItalic(true);

    const QFontMetricsF metrics(regular);
    m_cellWidth = std::max(1, static_cast<int>(std::ceil(metrics.horizontalAdvance(QLatin1Char('M')))));
    m_cellHeight = std::max(1, static_cast<int>(std::ceil(metrics.lineSpacing())));
    m_ascent = static_cast<int>(std::ceil(metrics.ascent()));
    // Runs of text are drawn with one drawText() call, so every glyph must advance by exactly one
    // cell; otherwise the fractional difference accumulates and text drifts off the grid.
    for (QFont& f : m_fonts) {
        const qreal advance = QFontMetricsF(f).horizontalAdvance(QLatin1Char('M'));
        f.setLetterSpacing(QFont::AbsoluteSpacing, m_cellWidth - advance);
    }
    const bool atBottom = isScrolledToBottom();
    updateGridSize();
    updateScrollBar(atBottom);
    viewport()->update();
}

QSize TerminalWidget::sizeForGrid(int rows, int columns) const {
    const int scrollBarWidth = verticalScrollBar()->sizeHint().width();
    return {columns * m_cellWidth + scrollBarWidth + 2 * frameWidth(), rows * m_cellHeight + 2 * frameWidth()};
}

QSize TerminalWidget::sizeHint() const {
    return sizeForGrid(24, 80);
}

bool TerminalWidget::isScrolledToBottom() const {
    return verticalScrollBar()->value() == verticalScrollBar()->maximum();
}

// ---------------------------------------------------------------------------------------------
// Session / scrolling

void TerminalWidget::onOutput() {
    if (!m_session) {
        return;
    }
    const bool atBottom = isScrolledToBottom();
    updateScrollBar(atBottom);
    Terminal& term = m_session->terminal();
    if (term.modes().synchronizedOutput) {
        // The application is mid-frame; paint once it ends the update (or the fallback fires).
        if (!m_syncTimer.isActive()) {
            m_syncTimer.start();
        }
        return;
    }
    m_syncTimer.stop();
    m_cursorBlinkOn = true;
    const bool wantBlink = term.cursor().blinking && hasFocus();
    if (wantBlink != m_blinkTimer.isActive()) {
        wantBlink ? m_blinkTimer.start() : m_blinkTimer.stop();
    }

    // Repaint only the rows that changed, plus where the cursor was and is.
    const Terminal::Damage damage = term.takeDamage();
    if (damage.full || !atBottom || m_wheelAccumulator != 0) {
        viewport()->update();
    } else {
        for (int row : damage.rows) {
            updateRow(row);
        }
        updateRow(m_lastCursorRow);
        updateRow(term.cursor().row);
    }
    m_lastCursorRow = term.cursor().row;
    if (m_hoverLink) {
        m_hoverLink.reset(); // the text under the pointer may have changed
        viewport()->unsetCursor();
        viewport()->setCursor(Qt::IBeamCursor);
    }
}

void TerminalWidget::onBell() {
    switch (m_appearance.bell) {
    case wt::session::BellStyle::None: break;
    case wt::session::BellStyle::Audible: QApplication::beep(); break;
    case wt::session::BellStyle::Visual:
        m_flashTimer.start();
        viewport()->update();
        break;
    }
    Q_EMIT bellRang();
}

void TerminalWidget::updateGridSize() {
    if (!m_session) {
        return;
    }
    const int columns = std::max(1, viewport()->width() / m_cellWidth);
    const int rows = std::max(1, viewport()->height() / m_cellHeight);
    m_session->resize(rows, columns, m_cellWidth, m_cellHeight);
}

void TerminalWidget::updateScrollBar(bool stickToBottom) {
    QScrollBar* bar = verticalScrollBar();
    int history = 0;
    int rows = 1;
    if (m_session) {
        const Terminal& term = m_session->terminal();
        history = term.isAlternateScreen() ? 0 : static_cast<int>(term.scrollbackSize());
        rows = term.rows();
    }
    const QSignalBlocker blocker(bar);
    bar->setRange(0, history);
    bar->setPageStep(rows);
    bar->setSingleStep(1);
    if (stickToBottom) {
        bar->setValue(history);
    }
}

void TerminalWidget::scrollToBottom() {
    verticalScrollBar()->setValue(verticalScrollBar()->maximum());
}

void TerminalWidget::scrollToRow(std::int64_t absoluteRow) {
    if (!m_session) {
        return;
    }
    const Terminal& term = m_session->terminal();
    if (term.isAlternateScreen()) {
        return;
    }
    const std::int64_t top = viewportTopRow();
    if (absoluteRow >= top && absoluteRow < top + term.rows()) {
        return; // already visible
    }
    const std::int64_t value = absoluteRow - term.historyStart() - term.rows() / 2;
    verticalScrollBar()->setValue(static_cast<int>(std::clamp<std::int64_t>(value, 0, verticalScrollBar()->maximum())));
}

void TerminalWidget::scrollContentsBy(int, int) {
    m_hoverLink.reset();
    viewport()->update();
}

std::int64_t TerminalWidget::viewportTopRow() const {
    const Terminal& term = m_session->terminal();
    if (term.isAlternateScreen()) {
        return term.screenStart();
    }
    const int history = static_cast<int>(term.scrollbackSize());
    return term.historyStart() + std::clamp(verticalScrollBar()->value(), 0, history);
}

QRect TerminalWidget::rowRect(int viewportRow) const {
    return {0, viewportRow * m_cellHeight, viewport()->width(), m_cellHeight};
}

void TerminalWidget::updateRow(int viewportRow) {
    viewport()->update(rowRect(viewportRow));
}

GridPoint TerminalWidget::gridPointAt(const QPoint& pos) const {
    if (!m_session) {
        return {};
    }
    const Terminal& term = m_session->terminal();
    const int row = std::clamp(pos.y() / m_cellHeight, 0, term.rows() - 1);
    int column = std::clamp(pos.x() / m_cellWidth, 0, term.columns() - 1);
    if (const Line* line = term.absoluteLine(viewportTopRow() + row); line && line->size != LineSize::Normal) {
        column = std::clamp(pos.x() / (2 * m_cellWidth), 0, term.columns() - 1);
    }
    return {viewportTopRow() + row, column};
}

// ---------------------------------------------------------------------------------------------
// Painting

QColor TerminalWidget::resolveColor(const Color& color, bool foreground, bool bold) const {
    return fromRgb(m_session->terminal().palette().resolve(color, foreground, bold));
}

void TerminalWidget::setSearchResults(std::vector<SearchMatch> matches, int current) {
    m_searchMatches = std::move(matches);
    m_currentMatch = current >= 0 && current < static_cast<int>(m_searchMatches.size()) ? current : -1;
    viewport()->update();
}

void TerminalWidget::setHighlightRules(std::vector<CompiledTrigger> rules) {
    m_highlightRules.clear();
    for (auto& rule : rules) {
        if (rule.rule.action == wt::session::TriggerAction::Highlight) {
            m_highlightRules.push_back(std::move(rule));
        }
    }
    viewport()->update();
}

std::vector<TerminalWidget::RuleMatch> TerminalWidget::highlightMatches(std::int64_t absoluteRow) const {
    std::vector<RuleMatch> result;
    if (m_highlightRules.empty() || !m_session) {
        return result;
    }
    const Terminal& term = m_session->terminal();
    const Line* line = term.absoluteLine(absoluteRow);
    if (!line) {
        return result;
    }
    // The row as text, with the cell column of every UTF-16 unit.
    QString text;
    std::vector<int> columnAt;
    const int columns = std::min<int>(static_cast<int>(line->cells.size()), term.columns());
    for (int col = 0; col < columns; ++col) {
        const Cell& cell = line->cells[static_cast<std::size_t>(col)];
        if (cell.width == 0 || cell.attributes.has(WidePadding)) {
            continue;
        }
        const QString piece = cellString(term, cell);
        text += piece;
        columnAt.insert(columnAt.end(), static_cast<std::size_t>(piece.size()), col);
    }
    for (std::size_t i = 0; i < m_highlightRules.size(); ++i) {
        auto it = m_highlightRules[i].regex.globalMatch(text);
        while (it.hasNext()) {
            const auto match = it.next();
            if (match.capturedLength() <= 0) {
                continue;
            }
            const int first = columnAt[static_cast<std::size_t>(match.capturedStart())];
            const int last = columnAt[static_cast<std::size_t>(match.capturedEnd() - 1)];
            const int width = line->cells[static_cast<std::size_t>(last)].width;
            result.push_back({first, last + std::max(0, width - 1), static_cast<int>(i)});
        }
    }
    return result;
}

std::vector<TerminalWidget::RowHighlight> TerminalWidget::searchHighlightsForRow(std::int64_t absoluteRow) const {
    std::vector<RowHighlight> result;
    for (const RuleMatch& match : highlightMatches(absoluteRow)) {
        result.push_back({match.startColumn, match.endColumn, Highlight::Rule, match.rule});
    }
    if (m_searchMatches.empty()) {
        return result;
    }
    // Matches are sorted by start; a match spanning rows starts at most a few rows earlier.
    auto it = std::lower_bound(m_searchMatches.begin(), m_searchMatches.end(), absoluteRow,
                               [](const SearchMatch& m, std::int64_t row) { return m.end.row < row; });
    const int columns = m_session->terminal().columns();
    for (; it != m_searchMatches.end() && it->start.row <= absoluteRow; ++it) {
        if (it->end.row < absoluteRow) {
            continue;
        }
        const int from = it->start.row == absoluteRow ? it->start.column : 0;
        const int to = it->end.row == absoluteRow ? it->end.column : columns - 1;
        const bool current = (it - m_searchMatches.begin()) == m_currentMatch;
        result.push_back({from, to, current ? Highlight::CurrentMatch : Highlight::Match});
    }
    return result;
}

void TerminalWidget::paintEvent(QPaintEvent* event) {
    QPainter painter(viewport());
    if (!m_session) {
        painter.fillRect(event->rect(), fromRgb(m_appearance.theme.palette.background));
        return;
    }
    const Terminal& term = m_session->terminal();
    const ColorPalette& palette = term.palette();
    const bool reverse = term.modes().reverseVideo;
    painter.fillRect(event->rect(), fromRgb(reverse ? palette.foreground : palette.background));

    const auto selection = term.selectionRange();
    const std::int64_t top = viewportTopRow();
    const int firstRow = std::max(0, event->rect().top() / m_cellHeight);
    const int lastRow = std::min(term.rows() - 1, event->rect().bottom() / m_cellHeight);
    const auto images = term.visibleImages();
    pruneImageCache(images);
    drawImages(painter, images, top, firstRow, lastRow, true); // negative z-index: under the text
    for (int row = firstRow; row <= lastRow; ++row) {
        const Line* line = term.absoluteLine(top + row);
        if (!line) {
            continue;
        }
        const auto highlights = searchHighlightsForRow(top + row);
        const int y = row * m_cellHeight;
        if (line->size == LineSize::Normal) {
            drawLine(painter, *line, y, top + row, selection, highlights);
            continue;
        }
        // DECDWL / DECDHL: draw the line at twice the width (and height), clipped to its row.
        painter.save();
        painter.setClipRect(rowRect(row));
        const bool doubleHeight = line->size != LineSize::DoubleWidth;
        painter.translate(0, line->size == LineSize::DoubleHeightBottom ? y - m_cellHeight : y);
        painter.scale(2.0, doubleHeight ? 2.0 : 1.0);
        drawLine(painter, *line, 0, top + row, selection, highlights);
        painter.restore();
    }
    drawImages(painter, images, top, firstRow, lastRow, false);
    drawCursor(painter);
    if (m_flashTimer.isActive()) {
        QColor flash = fromRgb(palette.foreground);
        flash.setAlpha(60);
        painter.fillRect(viewport()->rect(), flash);
    }
}

void TerminalWidget::pruneImageCache(const std::vector<const Terminal::Image*>& images) const {
    // Keep the converted images of the bitmaps that are still shown.
    for (auto it = m_imageCache.begin(); it != m_imageCache.end();) {
        const bool shown = std::any_of(images.begin(), images.end(),
                                       [&](const Terminal::Image* image) { return image->bitmap.get() == it.key(); });
        it = shown && !it->source.expired() ? std::next(it) : m_imageCache.erase(it);
    }
}

const QImage& TerminalWidget::cachedImage(const std::shared_ptr<const Bitmap>& bitmap) const {
    auto it = m_imageCache.find(bitmap.get());
    if (it == m_imageCache.end() || it->source.lock() != bitmap) { // (a new bitmap at a freed address)
        const QImage view(reinterpret_cast<const uchar*>(bitmap->pixels.data()), bitmap->width, bitmap->height,
                          bitmap->width * 4, QImage::Format_ARGB32);
        // A converted (deep) copy: premultiplied is what QPainter blends fastest.
        it = m_imageCache.insert(bitmap.get(), {bitmap, view.convertToFormat(QImage::Format_ARGB32_Premultiplied)});
    }
    return it->image;
}

void TerminalWidget::drawImages(QPainter& painter, const std::vector<const Terminal::Image*>& images, std::int64_t top,
                                int firstRow, int lastRow, bool belowText) const {
    for (const Terminal::Image* image : images) {
        const std::int64_t first = image->row - top; // viewport row of the top edge
        if ((image->z < 0) != belowText || first + image->rows <= firstRow || first > lastRow) {
            continue;
        }
        // Scale with the font: the image was laid out for the cell size at that time.
        const double sx = static_cast<double>(m_cellWidth) / image->cellWidth;
        const double sy = static_cast<double>(m_cellHeight) / image->cellHeight;
        const QRectF target(image->column * m_cellWidth + image->offsetX * sx,
                            static_cast<double>(first * m_cellHeight) + image->offsetY * sy, image->pixelWidth * sx,
                            image->pixelHeight * sy);
        const QRectF source(image->sourceX, image->sourceY, image->sourceWidth, image->sourceHeight);
        const bool scaled = target.width() != source.width() || target.height() != source.height();
        painter.setRenderHint(QPainter::SmoothPixmapTransform, scaled);
        painter.drawImage(target, cachedImage(image->bitmap), source);
    }
    painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
}

void TerminalWidget::drawLine(QPainter& painter, const Line& line, int y, std::int64_t absoluteRow,
                              const std::optional<SelectionRange>& selection,
                              const std::vector<RowHighlight>& highlights) const {
    const Terminal& term = m_session->terminal();
    const bool reverse = term.modes().reverseVideo;
    const int columns = std::min<int>(static_cast<int>(line.cells.size()), term.columns());
    auto highlightAt = [&](int column) -> std::pair<Highlight, int> {
        if (selection && selection->contains(absoluteRow, column)) {
            return {Highlight::Selection, -1};
        }
        Highlight best = Highlight::None;
        int rule = -1;
        for (const RowHighlight& h : highlights) {
            if (column >= h.startColumn && column <= h.endColumn && h.kind > best) {
                best = h.kind;
                rule = h.rule;
            }
        }
        return {best, rule};
    };
    const bool hoverRow = m_hoverLink && m_hoverLink->row == absoluteRow;
    auto hovered = [&](int column) {
        return hoverRow && column >= m_hoverLink->startColumn && column <= m_hoverLink->endColumn;
    };

    int col = 0;
    while (col < columns) {
        const Cell& first = line.cells[static_cast<std::size_t>(col)];
        const CellAttributes& attr = first.attributes;
        const auto highlighted = highlightAt(col);
        const Highlight highlight = highlighted.first;
        const bool hover = hovered(col);

        // Group consecutive cells with identical attributes. Non-ASCII glyphs are drawn one per
        // cell so that font fallback with different advances cannot break the grid.
        const bool asciiRun = first.width == 1 && first.ch < 0x80;
        int end = col + std::max<int>(1, first.width);
        if (asciiRun) {
            while (end < columns) {
                const Cell& c = line.cells[static_cast<std::size_t>(end)];
                if (c.width != 1 || c.ch >= 0x80 || !(c.attributes == attr) || highlightAt(end) != highlighted ||
                    hovered(end) != hover) {
                    break;
                }
                ++end;
            }
        }
        end = std::min(end, columns);

        const bool bold = attr.has(Bold);
        QColor fg = resolveColor(attr.foreground, true, bold);
        QColor bg = resolveColor(attr.background, false, false);
        if (attr.has(Inverse) != reverse) {
            std::swap(fg, bg);
        }
        if (attr.has(Dim)) {
            fg = QColor((fg.red() + bg.red()) / 2, (fg.green() + bg.green()) / 2, (fg.blue() + bg.blue()) / 2);
        }
        if (attr.has(Hidden)) {
            fg = bg;
        }

        const QRect rect(col * m_cellWidth, y, (end - col) * m_cellWidth, m_cellHeight);
        const bool defaultBackground = attr.background.isDefault() && !attr.has(Inverse) && !reverse;
        switch (highlight) {
        case Highlight::Selection: painter.fillRect(rect, m_selectionBackground); break;
        case Highlight::CurrentMatch:
            painter.fillRect(rect, kCurrentMatchBackground);
            fg = Qt::black;
            break;
        case Highlight::Match: painter.fillRect(rect, kMatchBackground); break;
        case Highlight::Rule: {
            const CompiledTrigger& rule = m_highlightRules[static_cast<std::size_t>(highlighted.second)];
            if (rule.background.isValid()) {
                painter.fillRect(rect, rule.background);
            } else if (!defaultBackground) {
                painter.fillRect(rect, bg);
            }
            if (rule.foreground.isValid()) {
                fg = rule.foreground;
            }
            break;
        }
        case Highlight::None:
            if (!defaultBackground) {
                painter.fillRect(rect, bg);
            }
            break;
        }

        QString text;
        for (int i = col; i < end; ++i) {
            const Cell& c = line.cells[static_cast<std::size_t>(i)];
            if (c.width != 0) {
                text += cellString(term, c);
            }
        }
        if (!text.trimmed().isEmpty() && !(attr.has(Blink) && !m_cursorBlinkOn)) {
            painter.setFont(m_fonts[(bold ? 1 : 0) + (attr.has(Italic) ? 2 : 0)]);
            painter.setPen(fg);
            painter.drawText(QPointF(rect.left(), y + m_ascent), text);
        }
        drawDecorations(painter, attr, rect, fg);
        if (hover && attr.underlineStyle() == UnderlineStyle::None) {
            painter.fillRect(QRect(rect.left(), y + m_cellHeight - 2, rect.width(), 1), fg);
        }
        col = end;
    }
}

void TerminalWidget::drawDecorations(QPainter& painter, const CellAttributes& attr, const QRect& rect,
                                     const QColor& foreground) const {
    const UnderlineStyle style = attr.underlineStyle();
    if (style != UnderlineStyle::None) {
        const QColor color =
            attr.underlineColor.isDefault() ? foreground : resolveColor(attr.underlineColor, true, false);
        const int baseline = rect.top() + m_cellHeight - 2;
        switch (style) {
        case UnderlineStyle::None: break;
        case UnderlineStyle::Single: painter.fillRect(QRect(rect.left(), baseline, rect.width(), 1), color); break;
        case UnderlineStyle::Double:
            painter.fillRect(QRect(rect.left(), baseline - 2, rect.width(), 1), color);
            painter.fillRect(QRect(rect.left(), baseline, rect.width(), 1), color);
            break;
        case UnderlineStyle::Curly: {
            QPainterPath path;
            const qreal amplitude = std::max(1.0, m_cellHeight / 16.0);
            const qreal period = std::max(4.0, m_cellWidth / 2.0);
            path.moveTo(rect.left(), baseline);
            for (qreal x = rect.left(); x < rect.right() + 1; x += period / 2) {
                const bool up = static_cast<int>((x - rect.left()) / (period / 2)) % 2 == 0;
                path.quadTo(x + period / 4, baseline + (up ? -amplitude : amplitude), x + period / 2, baseline);
            }
            painter.save();
            painter.setRenderHint(QPainter::Antialiasing);
            painter.setPen(QPen(color, 1));
            painter.setBrush(Qt::NoBrush);
            painter.drawPath(path);
            painter.restore();
            break;
        }
        case UnderlineStyle::Dotted:
        case UnderlineStyle::Dashed: {
            painter.save();
            QPen pen(color, 1, style == UnderlineStyle::Dotted ? Qt::DotLine : Qt::DashLine);
            painter.setPen(pen);
            painter.drawLine(rect.left(), baseline, rect.right(), baseline);
            painter.restore();
            break;
        }
        }
    }
    if (attr.has(Strikethrough)) {
        painter.fillRect(QRect(rect.left(), rect.top() + m_cellHeight / 2, rect.width(), 1), foreground);
    }
    if (attr.has(Overline)) {
        painter.fillRect(QRect(rect.left(), rect.top(), rect.width(), 1), foreground);
    }
}

void TerminalWidget::drawCursor(QPainter& painter) const {
    const Terminal& term = m_session->terminal();
    const Cursor& cursor = term.cursor();
    if (!isScrolledToBottom() || !m_session->isRunning()) {
        return;
    }
    const ColorPalette& palette = term.palette();
    const QColor foreground = fromRgb(palette.foreground);
    const QColor background = fromRgb(palette.background);
    const QColor cursorColor = fromRgb(palette.cursor);
    const int x = cursor.column * m_cellWidth;
    const int y = cursor.row * m_cellHeight;

    if (!m_preedit.isEmpty()) {
        const QFontMetrics metrics(m_fonts[0]);
        const QRect rect(x, y, metrics.horizontalAdvance(m_preedit), m_cellHeight);
        painter.fillRect(rect, background);
        painter.setFont(m_fonts[0]);
        painter.setPen(foreground);
        painter.drawText(QPointF(x, y + m_ascent), m_preedit);
        painter.fillRect(QRect(x, y + m_cellHeight - 1, rect.width(), 1), foreground);
        return;
    }
    if (!cursor.visible) {
        return;
    }

    const Cell& cell = term.line(cursor.row).cells[static_cast<std::size_t>(cursor.column)];
    const int width = (cell.width == 2 ? 2 : 1) * m_cellWidth;
    const QRect rect(x, y, width, m_cellHeight);
    if (!hasFocus()) {
        painter.setPen(cursorColor);
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(rect.adjusted(0, 0, -1, -1));
        return;
    }
    if (cursor.blinking && !m_cursorBlinkOn) {
        return;
    }
    switch (cursor.shape) {
    case CursorShape::Block:
        painter.fillRect(rect, cursorColor);
        if (cell.width != 0 && cell.ch != U' ') {
            painter.setFont(m_fonts[cell.attributes.has(Bold) ? 1 : 0]);
            painter.setPen(background);
            painter.drawText(QPointF(x, y + m_ascent), cellString(term, cell));
        }
        break;
    case CursorShape::Underline: painter.fillRect(QRect(x, y + m_cellHeight - 2, width, 2), cursorColor); break;
    case CursorShape::Bar: painter.fillRect(QRect(x, y, 2, m_cellHeight), cursorColor); break;
    }
}

// ---------------------------------------------------------------------------------------------
// Links

std::optional<TerminalWidget::HoverLink> TerminalWidget::findLink(std::int64_t row, int column) const {
    const Terminal& term = m_session->terminal();
    const Line* line = term.absoluteLine(row);
    if (!line || column < 0 || column >= static_cast<int>(line->cells.size())) {
        return std::nullopt;
    }
    auto cellAt = [&](int c) -> const Cell& {
        const Cell* cell = &line->cells[static_cast<std::size_t>(c)];
        if (cell->width == 0 && c > 0) {
            cell = &line->cells[static_cast<std::size_t>(c - 1)];
        }
        return *cell;
    };

    // OSC 8 hyperlink: the run of cells carrying the same link id on this row.
    if (const std::uint16_t id = cellAt(column).attributes.hyperlink; id != 0) {
        HoverLink link;
        link.row = row;
        link.startColumn = column;
        link.endColumn = column;
        while (link.startColumn > 0 && cellAt(link.startColumn - 1).attributes.hyperlink == id) {
            --link.startColumn;
        }
        while (link.endColumn + 1 < static_cast<int>(line->cells.size()) &&
               cellAt(link.endColumn + 1).attributes.hyperlink == id) {
            ++link.endColumn;
        }
        link.url = QString::fromStdString(std::string(term.hyperlinkUri(id)));
        return link;
    }

    // Plain-text URL on this row.
    QString text;
    std::vector<int> columnOf; // UTF-16 index -> column
    for (int c = 0; c < static_cast<int>(line->cells.size()); ++c) {
        const Cell& cell = line->cells[static_cast<std::size_t>(c)];
        if (cell.width == 0) {
            continue;
        }
        const QString piece = cellString(term, cell);
        for (qsizetype i = 0; i < piece.size(); ++i) {
            columnOf.push_back(c);
        }
        text += piece;
    }
    static const QRegularExpression kUrl(
        QStringLiteral(R"((?:https?|ftp|file)://[^\s<>"'`]+|mailto:[^\s<>"'`]+)"));
    for (auto it = kUrl.globalMatch(text); it.hasNext();) {
        const QRegularExpressionMatch match = it.next();
        QString url = match.captured();
        // Trailing punctuation usually belongs to the surrounding sentence.
        while (!url.isEmpty() && QStringLiteral(".,;:!?'\"").contains(url.back())) {
            url.chop(1);
        }
        while (url.endsWith(QLatin1Char(')')) && url.count(QLatin1Char(')')) > url.count(QLatin1Char('('))) {
            url.chop(1);
        }
        if (url.isEmpty()) {
            continue;
        }
        const qsizetype start = match.capturedStart();
        const qsizetype last = start + url.size() - 1;
        const int startColumn = columnOf[static_cast<std::size_t>(start)];
        const int endColumn = columnOf[static_cast<std::size_t>(last)];
        if (column >= startColumn && column <= endColumn) {
            return HoverLink{row, startColumn, endColumn, url};
        }
    }
    return std::nullopt;
}

QString TerminalWidget::linkAt(const QPoint& pos) const {
    if (!m_session) {
        return {};
    }
    const GridPoint point = gridPointAt(pos);
    const auto link = findLink(point.row, point.column);
    return link ? link->url : QString();
}

void TerminalWidget::updateHover(const QPoint& pos, Qt::KeyboardModifiers modifiers) {
    std::optional<HoverLink> link;
    if (m_session && viewport()->rect().contains(pos)) {
        const GridPoint point = gridPointAt(pos);
        link = findLink(point.row, point.column);
    }
    const bool changed = link.has_value() != m_hoverLink.has_value() ||
                         (link && (link->row != m_hoverLink->row || link->startColumn != m_hoverLink->startColumn));
    m_hoverLink = link;
    if (changed) {
        viewport()->update();
    }
    viewport()->setCursor(link && (modifiers & kLinkModifier) ? Qt::PointingHandCursor : Qt::IBeamCursor);
    viewport()->setToolTip(link ? tr("%1\nCtrl+click to open").arg(link->url) : QString());
}

void TerminalWidget::activateLink(const QString& text) {
    const QUrl url(text);
    Q_EMIT linkActivated(url);
    if (m_openLinks && isSafeLinkScheme(url)) {
        QDesktopServices::openUrl(url);
    }
}

void TerminalWidget::leaveEvent(QEvent* event) {
    if (m_hoverLink) {
        m_hoverLink.reset();
        viewport()->update();
    }
    QAbstractScrollArea::leaveEvent(event);
}

// ---------------------------------------------------------------------------------------------
// Clipboard

void TerminalWidget::paste(const QString& text) {
    if (!m_session || text.isEmpty()) {
        return;
    }
    sendUserInput(encodePaste(text.toStdString(), m_session->terminal().modes().bracketedPaste));
    scrollToBottom();
}

QString TerminalWidget::selectedText() const {
    return m_session ? QString::fromStdString(m_session->terminal().selectedText()) : QString();
}

bool TerminalWidget::copySelection() {
    const QString text = selectedText();
    if (text.isEmpty()) {
        return false;
    }
    QApplication::clipboard()->setText(text, QClipboard::Clipboard);
    return true;
}

void TerminalWidget::copyToPrimarySelection() {
    QClipboard* clipboard = QApplication::clipboard();
    if (clipboard->supportsSelection()) {
        if (const QString text = selectedText(); !text.isEmpty()) {
            clipboard->setText(text, QClipboard::Selection);
        }
    }
}

void TerminalWidget::finishSelection() {
    copyToPrimarySelection();
    if (m_appearance.copyOnSelect) {
        copySelection();
    }
}

void TerminalWidget::selectAll() {
    if (!m_session) {
        return;
    }
    Terminal& term = m_session->terminal();
    const std::int64_t first = term.isAlternateScreen() ? term.screenStart() : term.historyStart();
    term.startSelection({first, 0}, SelectionMode::Character);
    term.extendSelection({term.screenStart() + term.rows() - 1, term.columns() - 1});
    copyToPrimarySelection();
    viewport()->update();
}

void TerminalWidget::clearScrollback() {
    if (!m_session) {
        return;
    }
    m_session->terminal().clearScrollback();
    m_searchMatches.clear();
    m_currentMatch = -1;
    updateScrollBar(true);
    viewport()->update();
}

// ---------------------------------------------------------------------------------------------
// Keyboard

bool TerminalWidget::handleShortcut(QKeyEvent* event) {
    const auto mods = event->modifiers() & ~Qt::KeypadModifier;
    const int key = event->key();
    if (m_builtinShortcuts) {
#ifdef Q_OS_MACOS
        if (mods == kAppCommand && key == Qt::Key_V) {
            paste(QApplication::clipboard()->text());
            return true;
        }
        if (mods == kAppCommand && key == Qt::Key_C) {
            copySelection();
            return true;
        }
        if (mods == kAppCommand && key == Qt::Key_A) {
            selectAll();
            return true;
        }
#else
        if ((mods == (Qt::ControlModifier | Qt::ShiftModifier) && key == Qt::Key_V) ||
            (mods == Qt::ShiftModifier && key == Qt::Key_Insert)) {
            paste(QApplication::clipboard()->text());
            return true;
        }
        if ((mods == (Qt::ControlModifier | Qt::ShiftModifier) && key == Qt::Key_C) ||
            (mods == Qt::ControlModifier && key == Qt::Key_Insert)) {
            copySelection();
            return true;
        }
        if (mods == (Qt::ControlModifier | Qt::ShiftModifier) && key == Qt::Key_A) {
            selectAll();
            return true;
        }
#endif
    }
    if (mods == Qt::ShiftModifier && (key == Qt::Key_PageUp || key == Qt::Key_PageDown)) {
        QScrollBar* bar = verticalScrollBar();
        bar->setValue(bar->value() + (key == Qt::Key_PageUp ? -bar->pageStep() : bar->pageStep()));
        return true;
    }
    if (mods == Qt::ShiftModifier && (key == Qt::Key_Home || key == Qt::Key_End)) {
        QScrollBar* bar = verticalScrollBar();
        bar->setValue(key == Qt::Key_Home ? bar->minimum() : bar->maximum());
        return true;
    }
    return false;
}

void TerminalWidget::keyPressEvent(QKeyEvent* event) {
    if (!m_session || !m_session->isRunning()) {
        event->ignore();
        return;
    }
    if (handleShortcut(event)) {
        return;
    }
    if (completionVisible() && (event->modifiers() & ~Qt::KeypadModifier) == Qt::NoModifier) {
        const int count = m_completion->count();
        const int current = m_completion->currentRow();
        switch (event->key()) {
        case Qt::Key_Down:
            m_completion->setCurrentRow((current + 1) % count);
            event->accept();
            return;
        case Qt::Key_Up:
            m_completion->setCurrentRow(current <= 0 ? count - 1 : current - 1);
            event->accept();
            return;
        case Qt::Key_Right:
        case Qt::Key_Tab:
        case Qt::Key_Return:
        case Qt::Key_Enter:
            if (current >= 0) { // only a suggestion the user picked replaces the key
                acceptCompletion(current);
                event->accept();
                return;
            }
            break;
        case Qt::Key_Escape:
            hideCompletion();
            event->accept();
            return;
        default: break;
        }
    }

    const std::string bytes = encodeKeyEvent(event, event->isAutoRepeat() ? KeyAction::Repeat : KeyAction::Press);
    if (bytes.empty()) {
        QAbstractScrollArea::keyPressEvent(event);
        return;
    }
    sendUserInput(bytes);
    m_cursorBlinkOn = true;
    scrollToBottom();
    event->accept();
}

void TerminalWidget::keyReleaseEvent(QKeyEvent* event) {
    // Only the kitty protocol's "report event types" mode wants releases; Qt's auto-repeat
    // releases are not real ones (repeats are reported as such on the press side).
    if (!m_session || !m_session->isRunning() || event->isAutoRepeat() ||
        !(m_session->terminal().kittyKeyboardFlags() & KittyReportEvents)) {
        QAbstractScrollArea::keyReleaseEvent(event);
        return;
    }
    const std::string bytes = encodeKeyEvent(event, KeyAction::Release);
    if (bytes.empty()) {
        QAbstractScrollArea::keyReleaseEvent(event);
        return;
    }
    m_session->sendInput(bytes); // escape codes for the application, not command-line text
    event->accept();
}

std::string TerminalWidget::encodeKeyEvent(const QKeyEvent* event, KeyAction action) const {
    const Terminal& term = m_session->terminal();
    const unsigned mods = toTerminalModifiers(event->modifiers());
    KeyboardModes modes;
    modes.cursorKeysApplication = term.modes().cursorKeysApplication;
    modes.newLineMode = term.modes().newLineMode;
    modes.keypadApplication = term.modes().keypadApplication;
    modes.kittyFlags = term.kittyKeyboardFlags();
    modes.modifyOtherKeys = term.modes().modifyOtherKeys;
    std::string bytes;

    const int key = event->key();
    // Modifier keys on their own (kitty "report all keys" only).
    std::optional<ModifierKey> modifierKey;
    switch (key) {
    case Qt::Key_Shift: modifierKey = ModifierKey::Shift; break;
#ifdef Q_OS_MACOS
    case Qt::Key_Control: modifierKey = ModifierKey::Super; break; // Command
    case Qt::Key_Meta: modifierKey = ModifierKey::Control; break;
#else
    case Qt::Key_Control: modifierKey = ModifierKey::Control; break;
    case Qt::Key_Meta:
    case Qt::Key_Super_L:
    case Qt::Key_Super_R: modifierKey = ModifierKey::Super; break;
#endif
    case Qt::Key_Alt:
    case Qt::Key_AltGr: modifierKey = ModifierKey::Alt; break;
    case Qt::Key_CapsLock: modifierKey = ModifierKey::CapsLock; break;
    case Qt::Key_NumLock: modifierKey = ModifierKey::NumLock; break;
    default: break;
    }
    if (modifierKey) {
        return encodeModifierKey(*modifierKey, mods, modes, action);
    }

    // Keypad digits and operators are distinct keys in DECKPAM and the kitty protocol. (Keypad
    // cursor keys stay ordinary cursor keys: macOS flags the main arrows as keypad keys too.)
    const bool keypadModes = modes.keypadApplication || (modes.kittyFlags & (KittyDisambiguate | KittyReportAllKeys));
    const auto keypadKey =
        (event->modifiers() & Qt::KeypadModifier) && keypadModes ? mapKeypadKey(event->key()) : std::nullopt;
    // What the key typed, for the kitty protocol's alternates/text fields.
    const QString text = event->text();
    const std::u32string typed = action == KeyAction::Release ? std::u32string() : text.toStdU32String();
    auto info = [&](char32_t base) {
        KeyEventInfo e;
        e.action = action;
        e.text = typed;
        if (typed.size() == 1 && typed.front() != base) {
            e.shifted = typed.front(); // e.g. 'A' for Shift+a
        }
        return e;
    };
    if (keypadKey) {
        bytes = encodeKey(*keypadKey, mods, modes, action);
    } else if (const auto mapped = mapKey(key)) {
        const unsigned keyMods = key == Qt::Key_Backtab ? (mods | ShiftModifier) : mods;
        bytes = encodeKey(*mapped, keyMods, modes, action);
    } else if ((mods & ControlModifier) && key >= Qt::Key_Space && key <= Qt::Key_AsciiTilde) {
        // Derive control codes from the key rather than text(): macOS does not translate them.
        bytes = encodeCharKey(static_cast<char32_t>(key), mods & (ControlModifier | AltModifier | ShiftModifier), modes,
                              info(static_cast<char32_t>(key)));
    } else if (action == KeyAction::Release) {
        // Releases carry no text: rebuild the key from its code (letters are reported unshifted).
        if (key >= Qt::Key_Space && key <= Qt::Key_AsciiTilde) {
            char32_t ch = static_cast<char32_t>(key);
            if (ch >= U'A' && ch <= U'Z') {
                ch = ch - U'A' + U'a';
            }
            bytes = encodeCharKey(ch, mods, modes, info(ch));
        } else if (!text.isEmpty()) {
            const char32_t ch = text.toUcs4().front();
            bytes = encodeCharKey(ch, mods, modes, info(ch));
        }
    } else {
#ifdef Q_OS_MACOS
        // Option composes characters on macOS unless "Option as Meta" is on.
        const bool optionAsMeta = m_appearance.optionAsMeta && (mods & AltModifier);
        if (optionAsMeta && key >= Qt::Key_Space && key <= Qt::Key_AsciiTilde) {
            char32_t ch = static_cast<char32_t>(key);
            if (ch >= U'A' && ch <= U'Z' && !(mods & ShiftModifier)) {
                ch = ch - U'A' + U'a';
            }
            bytes = encodeCharKey(ch, AltModifier, modes, info(ch));
        }
        const unsigned textMods = NoModifier;
#else
        const unsigned textMods = mods & AltModifier;
#endif
        if (bytes.empty()) {
            for (char32_t cp : text.toUcs4()) {
                if (cp < 0x20 || cp == 0x7F) {
                    bytes += encodeChar(cp, textMods); // control characters from the IME / layout
                } else {
                    const char32_t base = (cp >= U'A' && cp <= U'Z') ? cp - U'A' + U'a' : cp;
                    bytes += encodeCharKey(cp, textMods | (mods & ShiftModifier), modes, info(base));
                }
            }
        }
    }
    return bytes;
}

void TerminalWidget::sendUserInput(std::string_view bytes) {
    if (!m_session || bytes.empty()) {
        return;
    }
    const Terminal& term = m_session->terminal();
    if (term.isAlternateScreen()) {
        m_inputLine.reset(); // full-screen programs: nothing to remember
        m_promptEnd.reset();
        hideCompletion();
        m_session->sendInput(bytes);
        return;
    }
    if (m_inputLine.empty() && static_cast<unsigned char>(bytes.front()) >= 0x20 && bytes.front() != 0x7F) {
        m_promptEnd = GridPoint{term.screenStart() + term.cursor().row, term.cursor().column};
    }
    const auto submitted = m_inputLine.feed(bytes);
    if (!submitted.empty()) {
        hideCompletion();
        const auto& marks = term.shellMarks();
        const std::optional<GridPoint> start =
            marks.commandStart && !marks.commandRunning && (!m_promptEnd || marks.commandStart->row >= m_promptEnd->row)
                ? marks.commandStart
                : m_promptEnd;
        if (start && submitted.front().reliable) {
            // Only what the shell echoed counts: input typed at a silent password prompt is never
            // recorded. Give the echo a moment to arrive over slow links.
            const QString typed = QString::fromStdString(submitted.front().text);
            const GridPoint at = *start;
            QPointer<TerminalWidget> self(this);
            QTimer::singleShot(300, this, [self, typed, at] {
                if (!self || !self->m_session) {
                    return;
                }
                const QString screen = QString::fromStdString(self->m_session->terminal().textFrom(at));
                if (!typed.trimmed().isEmpty() && screen.startsWith(typed.trimmed())) {
                    Q_EMIT self->commandSubmitted(typed);
                }
            });
        } else if (start) {
            // Edited with cursor keys or recalled from the shell history: take the screen's word.
            const QString screen = QString::fromStdString(term.textFrom(*start)).trimmed();
            if (!screen.isEmpty()) {
                Q_EMIT commandSubmitted(screen);
            }
        }
        m_promptEnd.reset();
        if (!m_inputLine.empty()) {
            m_promptEnd = GridPoint{term.screenStart() + term.cursor().row, term.cursor().column};
        }
    }
    m_session->sendInput(bytes);
    if (m_completionProvider) {
        updateCompletion();
    }
}

void TerminalWidget::setCompletionProvider(CompletionProvider provider, int minimumPrefix) {
    m_completionProvider = std::move(provider);
    m_completionMinimum = std::max(1, minimumPrefix);
    if (!m_completionProvider) {
        hideCompletion();
    }
}

bool TerminalWidget::completionVisible() const {
    return m_completion && m_completion->isVisible() && m_completion->count() > 0;
}

QStringList TerminalWidget::completionItems() const {
    QStringList items;
    if (m_completion) {
        for (int i = 0; i < m_completion->count(); ++i) {
            items << m_completion->item(i)->text();
        }
    }
    return items;
}

void TerminalWidget::hideCompletion() {
    if (m_completion) {
        m_completion->hide();
    }
}

void TerminalWidget::updateCompletion() {
    const std::string& typed = m_inputLine.line();
    if (!m_completionProvider || !m_session || !m_inputLine.reliable() || m_session->terminal().isAlternateScreen() ||
        static_cast<int>(QString::fromStdString(typed).size()) < m_completionMinimum) {
        hideCompletion();
        return;
    }
    const QStringList items = m_completionProvider(QString::fromStdString(typed));
    if (items.isEmpty()) {
        hideCompletion();
        return;
    }
    if (!m_completion) {
        m_completion = new QListWidget(this);
        m_completion->setObjectName(QStringLiteral("completionPopup"));
        m_completion->setWindowFlags(Qt::ToolTip | Qt::FramelessWindowHint);
        m_completion->setAttribute(Qt::WA_ShowWithoutActivating);
        m_completion->setFocusPolicy(Qt::NoFocus);
        m_completion->setFont(m_fonts[0]);
        m_completion->setUniformItemSizes(true);
        connect(m_completion, &QListWidget::itemClicked, this,
                [this](QListWidgetItem* item) { acceptCompletion(m_completion->row(item)); });
    }
    m_completion->clear();
    m_completion->addItems(items);
    m_completion->setCurrentRow(-1);
    const QFontMetrics metrics(m_completion->font());
    int width = 0;
    for (const QString& item : items) {
        width = std::max(width, metrics.horizontalAdvance(item));
    }
    const int rows = std::min<int>(static_cast<int>(items.size()), 8);
    const int rowHeight = std::max(metrics.height(), m_completion->sizeHintForRow(0));
    m_completion->resize(std::min(width + 24, std::max(200, viewport()->width())), rows * rowHeight + 6);
    const Terminal& term = m_session->terminal();
    const std::int64_t cursorRow = term.screenStart() + term.cursor().row - viewportTopRow();
    const QPoint below(term.cursor().column * m_cellWidth, static_cast<int>(cursorRow + 1) * m_cellHeight);
    m_completion->move(viewport()->mapToGlobal(below));
    m_completion->show();
}

void TerminalWidget::acceptCompletion(int row) {
    if (!m_completion || row < 0 || row >= m_completion->count()) {
        return;
    }
    const QString choice = m_completion->item(row)->text();
    const QString typed = QString::fromStdString(m_inputLine.line());
    hideCompletion();
    if (choice.startsWith(typed) && choice.size() > typed.size()) {
        const std::string rest = choice.mid(typed.size()).toStdString();
        sendUserInput(rest);
        hideCompletion(); // the inserted text may match more entries; do not reopen at once
    }
}

void TerminalWidget::inputMethodEvent(QInputMethodEvent* event) {
    if (m_session && !event->commitString().isEmpty()) {
        sendUserInput(event->commitString().toStdString());
        scrollToBottom();
    }
    m_preedit = event->preeditString();
    viewport()->update();
    event->accept();
}

QVariant TerminalWidget::inputMethodQuery(Qt::InputMethodQuery query) const {
    if (!m_session) {
        return QAbstractScrollArea::inputMethodQuery(query);
    }
    const Cursor& cursor = m_session->terminal().cursor();
    const QRect cursorRect(cursor.column * m_cellWidth, cursor.row * m_cellHeight, m_cellWidth, m_cellHeight);
    switch (query) {
    case Qt::ImCursorRectangle: return cursorRect.translated(viewport()->pos());
    case Qt::ImFont: return m_fonts[0];
    case Qt::ImEnabled: return true;
    default: return QAbstractScrollArea::inputMethodQuery(query);
    }
}

void TerminalWidget::focusInEvent(QFocusEvent* event) {
    if (m_session && m_session->terminal().modes().focusEvents) {
        m_session->sendBytes("\x1b[I");
    }
    if (m_session && m_session->terminal().cursor().blinking) {
        m_cursorBlinkOn = true;
        m_blinkTimer.start();
    }
    viewport()->update();
    QAbstractScrollArea::focusInEvent(event);
    Q_EMIT focusReceived();
}

void TerminalWidget::focusOutEvent(QFocusEvent* event) {
    if (m_session && m_session->terminal().modes().focusEvents) {
        m_session->sendBytes("\x1b[O");
    }
    if (event->reason() != Qt::PopupFocusReason) {
        hideCompletion();
    }
    m_blinkTimer.stop();
    m_cursorBlinkOn = true;
    viewport()->update();
    QAbstractScrollArea::focusOutEvent(event);
}

bool TerminalWidget::focusNextPrevChild(bool) {
    return false; // Tab / Shift+Tab belong to the terminal
}

// ---------------------------------------------------------------------------------------------
// Mouse

bool TerminalWidget::reportsMouse(Qt::KeyboardModifiers modifiers) const {
    // Shift always gives the mouse back to the terminal (selection, context menu), like xterm.
    return m_session && m_session->isRunning() &&
           m_session->terminal().modes().mouseTracking != MouseTracking::None && !(modifiers & Qt::ShiftModifier);
}

bool TerminalWidget::sendMouseEvent(MouseAction action, MouseButton button, const QPoint& pos,
                                    Qt::KeyboardModifiers modifiers) {
    const Terminal& term = m_session->terminal();
    MouseEvent event;
    event.action = action;
    event.button = button;
    event.modifiers = toTerminalModifiers(modifiers);
    event.row = std::clamp(pos.y() / m_cellHeight, 0, term.rows() - 1);
    event.column = std::clamp(pos.x() / m_cellWidth, 0, term.columns() - 1);
    if (action == MouseAction::Motion) {
        const QPoint cell(event.column, event.row);
        if (cell == m_lastReportedCell) {
            return true; // only report motion into a new cell
        }
        m_lastReportedCell = cell;
    } else {
        m_lastReportedCell = QPoint(event.column, event.row);
    }
    const std::string bytes = encodeMouse(event, term.modes().mouseTracking, term.modes().mouseEncoding);
    if (!bytes.empty()) {
        m_session->sendBytes(bytes);
    }
    return true;
}

void TerminalWidget::mousePressEvent(QMouseEvent* event) {
    setFocus(Qt::MouseFocusReason);
    if (!m_session) {
        QAbstractScrollArea::mousePressEvent(event);
        return;
    }
    const QPoint pos = event->position().toPoint();
    if (event->button() == Qt::LeftButton && (event->modifiers() & kLinkModifier) &&
        !(event->modifiers() & Qt::ShiftModifier)) {
        if (const QString link = linkAt(pos); !link.isEmpty()) {
            activateLink(link);
            event->accept();
            return;
        }
    }
    if (reportsMouse(event->modifiers())) {
        if (const auto button = mapButton(event->button())) {
            m_reportedButton = *button;
            sendMouseEvent(MouseAction::Press, *button, pos, event->modifiers());
        }
        event->accept();
        return;
    }

    Terminal& term = m_session->terminal();
    if (event->button() == Qt::LeftButton) {
        const GridPoint point = gridPointAt(pos);
        const bool tripleClick = m_lastDoubleClick.isValid() &&
                                 m_lastDoubleClick.elapsed() < QGuiApplication::styleHints()->mouseDoubleClickInterval() &&
                                 (pos - m_doubleClickPos).manhattanLength() <= QGuiApplication::styleHints()->startDragDistance();
        m_lastDoubleClick.invalidate();
        if (tripleClick) {
            term.startSelection(point, SelectionMode::Line);
            m_dragMode = SelectionMode::Line;
            m_selecting = true;
        } else if ((event->modifiers() & Qt::ShiftModifier) && term.hasSelection()) {
            term.extendSelection(point);
            m_selecting = true;
        } else {
            term.clearSelection();
            m_pressPos = pos;
            m_pressPoint = point;
            m_dragMode = (event->modifiers() & Qt::AltModifier) ? SelectionMode::Block : SelectionMode::Character;
            m_selecting = true;
        }
        m_lastDragPos = pos;
        viewport()->update();
        event->accept();
        return;
    }
    if (event->button() == Qt::MiddleButton) {
        QClipboard* clipboard = QApplication::clipboard();
        if (clipboard->supportsSelection()) {
            paste(clipboard->text(QClipboard::Selection));
        }
        event->accept();
        return;
    }
    QAbstractScrollArea::mousePressEvent(event);
}

void TerminalWidget::mouseDoubleClickEvent(QMouseEvent* event) {
    if (!m_session || reportsMouse(event->modifiers()) || event->button() != Qt::LeftButton) {
        // Applications see a double click as two presses.
        mousePressEvent(event);
        return;
    }
    const QPoint pos = event->position().toPoint();
    m_session->terminal().startSelection(gridPointAt(pos), SelectionMode::Word);
    m_dragMode = SelectionMode::Word;
    m_selecting = true;
    m_lastDragPos = pos;
    m_lastDoubleClick.start();
    m_doubleClickPos = pos;
    finishSelection();
    viewport()->update();
    event->accept();
}

void TerminalWidget::updateSelectionDrag(const QPoint& pos) {
    Terminal& term = m_session->terminal();
    m_lastDragPos = pos;
    if (!term.hasSelection()) {
        // A plain click must not select: start once the pointer leaves the cell (or moves more
        // than half a cell within it).
        const GridPoint point = gridPointAt(pos);
        if (point == m_pressPoint && std::abs(pos.x() - m_pressPos.x()) < m_cellWidth / 2) {
            return;
        }
        term.startSelection(m_pressPoint, m_dragMode);
    }
    term.extendSelection(gridPointAt(pos));
    const bool outside = pos.y() < 0 || pos.y() >= viewport()->height();
    if (outside && !m_autoScrollTimer.isActive()) {
        m_autoScrollTimer.start();
    } else if (!outside) {
        m_autoScrollTimer.stop();
    }
    viewport()->update();
}

void TerminalWidget::autoScrollSelection() {
    if (!m_selecting || !m_session) {
        m_autoScrollTimer.stop();
        return;
    }
    QScrollBar* bar = verticalScrollBar();
    if (m_lastDragPos.y() < 0) {
        bar->setValue(bar->value() - 1);
    } else if (m_lastDragPos.y() >= viewport()->height()) {
        bar->setValue(bar->value() + 1);
    } else {
        m_autoScrollTimer.stop();
    }
    if (m_session->terminal().hasSelection()) {
        m_session->terminal().extendSelection(gridPointAt(m_lastDragPos));
        viewport()->update();
    }
}

void TerminalWidget::mouseMoveEvent(QMouseEvent* event) {
    if (!m_session) {
        QAbstractScrollArea::mouseMoveEvent(event);
        return;
    }
    const QPoint pos = event->position().toPoint();
    if (m_selecting && (event->buttons() & Qt::LeftButton)) {
        updateSelectionDrag(pos);
        event->accept();
        return;
    }
    if (reportsMouse(event->modifiers())) {
        sendMouseEvent(MouseAction::Motion, m_reportedButton.value_or(MouseButton::None), pos, event->modifiers());
        event->accept();
        return;
    }
    if (event->buttons() == Qt::NoButton) {
        updateHover(pos, event->modifiers());
    }
    QAbstractScrollArea::mouseMoveEvent(event);
}

void TerminalWidget::mouseReleaseEvent(QMouseEvent* event) {
    if (!m_session) {
        QAbstractScrollArea::mouseReleaseEvent(event);
        return;
    }
    const QPoint pos = event->position().toPoint();
    if (m_selecting && event->button() == Qt::LeftButton) {
        m_selecting = false;
        m_autoScrollTimer.stop();
        if (m_session->terminal().hasSelection()) {
            finishSelection();
        }
        event->accept();
        return;
    }
    if (m_reportedButton && mapButton(event->button()) == m_reportedButton) {
        const MouseButton button = *m_reportedButton;
        m_reportedButton.reset();
        if (reportsMouse(event->modifiers() & ~Qt::ShiftModifier)) {
            sendMouseEvent(MouseAction::Release, button, pos, event->modifiers());
        }
        event->accept();
        return;
    }
    QAbstractScrollArea::mouseReleaseEvent(event);
}

void TerminalWidget::wheelEvent(QWheelEvent* event) {
    if (!m_session) {
        QAbstractScrollArea::wheelEvent(event);
        return;
    }
    if ((event->modifiers() & Qt::ControlModifier) && !reportsMouse(event->modifiers())) {
        // Ctrl+wheel zooms like in browsers and other terminals.
        m_wheelAccumulator += event->angleDelta().y();
        while (std::abs(m_wheelAccumulator) >= 120) {
            m_wheelAccumulator > 0 ? zoomIn() : zoomOut();
            m_wheelAccumulator -= m_wheelAccumulator > 0 ? 120 : -120;
        }
        event->accept();
        return;
    }
    const Terminal& term = m_session->terminal();
    const bool report = reportsMouse(event->modifiers());
    const bool alternateScroll =
        !report && term.isAlternateScreen() && term.modes().alternateScroll && !(event->modifiers() & Qt::ShiftModifier);
    if (!report && !alternateScroll) {
        QAbstractScrollArea::wheelEvent(event); // scroll the history
        return;
    }
    // One notch (120 units) per report; trackpads deliver smaller deltas that accumulate.
    m_wheelAccumulator += event->angleDelta().y();
    const int steps = m_wheelAccumulator / 120;
    m_wheelAccumulator %= 120;
    const QPoint pos = event->position().toPoint();
    for (int i = 0; i < std::abs(steps); ++i) {
        if (report) {
            sendMouseEvent(MouseAction::Press, steps > 0 ? MouseButton::WheelUp : MouseButton::WheelDown, pos,
                           event->modifiers());
        } else {
            KeyboardModes modes;
            modes.cursorKeysApplication = term.modes().cursorKeysApplication;
            const std::string arrow = encodeKey(steps > 0 ? Key::Up : Key::Down, NoModifier, modes);
            std::string bytes;
            for (int line = 0; line < QGuiApplication::styleHints()->wheelScrollLines(); ++line) {
                bytes += arrow;
            }
            m_session->sendBytes(bytes);
        }
    }
    event->accept();
}

void TerminalWidget::contextMenuEvent(QContextMenuEvent* event) {
    if (!m_session || reportsMouse(event->modifiers())) {
        event->accept(); // the right button belongs to the application
        return;
    }
    QMenu menu(this);
    const QString link = linkAt(event->pos());
    if (!link.isEmpty()) {
        menu.addAction(tr("Open Link"), this, [this, link] { activateLink(link); });
        menu.addAction(tr("Copy Link Address"), this,
                       [link] { QApplication::clipboard()->setText(link, QClipboard::Clipboard); });
        menu.addSeparator();
    }
    QAction* copy = menu.addAction(tr("Copy"), this, [this] { copySelection(); });
    copy->setEnabled(m_session->terminal().hasSelection());
    QAction* pasteAction =
        menu.addAction(tr("Paste"), this, [this] { paste(QApplication::clipboard()->text()); });
    pasteAction->setEnabled(m_session->isRunning() && !QApplication::clipboard()->text().isEmpty());
    menu.addSeparator();
    menu.addAction(tr("Select All"), this, &TerminalWidget::selectAll);
    menu.addAction(tr("Clear Scrollback"), this, &TerminalWidget::clearScrollback);
    Q_EMIT contextMenuAboutToShow(&menu);
    menu.exec(event->globalPos());
}

void TerminalWidget::resizeEvent(QResizeEvent* event) {
    QAbstractScrollArea::resizeEvent(event);
    const bool atBottom = isScrolledToBottom();
    updateGridSize();
    updateScrollBar(atBottom);
}
