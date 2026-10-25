#include "SearchBar.h"

#include "TerminalSession.h"
#include "TerminalWidget.h"

#include "wt/terminal/Terminal.h"

#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QToolButton>

using namespace wt::terminal;

wt::terminal::SearchMatcher qtRegexMatcher(const QRegularExpression& regex) {
    return [regex](std::string_view text, const std::function<bool(std::size_t, std::size_t)>& emit) {
        const QString subject = QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
        // UTF-16 index -> UTF-8 byte offset.
        std::vector<std::size_t> offsets;
        offsets.reserve(static_cast<std::size_t>(subject.size()) + 1);
        std::size_t bytes = 0;
        for (qsizetype i = 0; i < subject.size(); ++i) {
            offsets.push_back(bytes);
            const char16_t unit = subject[i].unicode();
            if (unit < 0x80) {
                bytes += 1;
            } else if (unit < 0x800) {
                bytes += 2;
            } else if (QChar::isHighSurrogate(unit)) {
                bytes += 4;
                offsets.push_back(bytes - 4); // the low surrogate maps to the same character
                ++i;
            } else {
                bytes += 3;
            }
        }
        offsets.push_back(bytes);
        for (auto it = regex.globalMatch(subject); it.hasNext();) {
            const auto match = it.next();
            if (match.capturedLength() == 0) {
                continue;
            }
            const auto begin = offsets[static_cast<std::size_t>(match.capturedStart())];
            const auto end = offsets[static_cast<std::size_t>(match.capturedEnd())];
            if (!emit(begin, end)) {
                return;
            }
        }
    };
}

namespace {

QToolButton* makeButton(const QString& text, const QString& tip, bool checkable, QWidget* parent) {
    auto* button = new QToolButton(parent);
    button->setText(text);
    button->setToolTip(tip);
    button->setCheckable(checkable);
    button->setAutoRaise(true);
    button->setFocusPolicy(Qt::NoFocus);
    return button;
}

} // namespace

SearchBar::SearchBar(TerminalWidget* terminal, QWidget* parent)
    : QWidget(parent),
      m_terminal(terminal),
      m_edit(new QLineEdit(this)),
      m_case(makeButton(QStringLiteral("Aa"), tr("Match case"), true, this)),
      m_word(makeButton(QStringLiteral("W"), tr("Whole words"), true, this)),
      m_regex(makeButton(QStringLiteral(".*"), tr("Regular expression"), true, this)),
      m_label(new QLabel(this)) {
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(4, 2, 4, 2);
    layout->setSpacing(2);
    m_edit->setPlaceholderText(tr("Find in scrollback"));
    m_edit->setClearButtonEnabled(true);
    m_edit->installEventFilter(this);
    layout->addWidget(m_edit, 1);
    layout->addWidget(m_case);
    layout->addWidget(m_word);
    layout->addWidget(m_regex);
    layout->addWidget(m_label);
    QToolButton* previous = makeButton(QStringLiteral("↑"), tr("Previous match (Enter)"), false, this);
    QToolButton* next = makeButton(QStringLiteral("↓"), tr("Next match (Shift+Enter)"), false, this);
    QToolButton* close = makeButton(QStringLiteral("✕"), tr("Close (Esc)"), false, this);
    layout->addWidget(previous);
    layout->addWidget(next);
    layout->addWidget(close);

    m_debounce.setSingleShot(true);
    m_debounce.setInterval(120);
    connect(&m_debounce, &QTimer::timeout, this, [this] { runSearch(false); });
    m_refresh.setSingleShot(true);
    m_refresh.setInterval(300);
    connect(&m_refresh, &QTimer::timeout, this, [this] { runSearch(true); });

    connect(m_edit, &QLineEdit::textChanged, this, [this] { m_debounce.start(); });
    for (QToolButton* toggle : {m_case, m_word, m_regex}) {
        connect(toggle, &QToolButton::toggled, this, [this] { runSearch(false); });
    }
    connect(previous, &QToolButton::clicked, this, &SearchBar::findPrevious);
    connect(next, &QToolButton::clicked, this, &SearchBar::findNext);
    connect(close, &QToolButton::clicked, this, [this] {
        hide();
        Q_EMIT closed();
    });
    if (m_terminal) {
        connect(m_terminal, &QObject::destroyed, this, [this] { m_terminal = nullptr; });
    }
    if (m_terminal && m_terminal->session()) {
        connect(m_terminal->session(), &TerminalSession::outputReceived, this, [this] {
            if (isVisible() && !m_edit->text().isEmpty() && !m_refresh.isActive()) {
                m_refresh.start();
            }
        });
    }
    updateLabel();
}

void SearchBar::activate() {
    show();
    m_edit->setFocus(Qt::ShortcutFocusReason);
    m_edit->selectAll();
    if (!m_edit->text().isEmpty()) {
        runSearch(true);
    }
}

void SearchBar::setQuery(const QString& text) {
    m_edit->setText(text);
    m_debounce.stop();
    runSearch(false);
}

QString SearchBar::query() const {
    return m_edit->text();
}

void SearchBar::setOptions(bool caseSensitive, bool wholeWord, bool regex) {
    const QSignalBlocker b1(m_case), b2(m_word), b3(m_regex);
    m_case->setChecked(caseSensitive);
    m_word->setChecked(wholeWord);
    m_regex->setChecked(regex);
    runSearch(false);
}

int SearchBar::matchCount() const {
    return m_terminal ? static_cast<int>(m_terminal->searchResults().size()) : 0;
}

void SearchBar::refresh() {
    runSearch(true);
}

void SearchBar::runSearch(bool keepPosition) {
    if (!m_terminal || !m_terminal->session()) {
        return;
    }
    m_error.clear();
    const QString text = m_edit->text();
    std::optional<SearchMatch> previous;
    if (keepPosition && m_current >= 0 && m_current < matchCount()) {
        previous = m_terminal->searchResults()[static_cast<std::size_t>(m_current)];
    }
    if (text.isEmpty()) {
        m_current = -1;
        m_terminal->setSearchResults({}, -1);
        updateLabel();
        return;
    }
    SearchMatcher matcher;
    if (m_regex->isChecked()) {
        QRegularExpression regex(text, m_case->isChecked() ? QRegularExpression::NoPatternOption
                                                           : QRegularExpression::CaseInsensitiveOption);
        if (!regex.isValid()) {
            m_error = regex.errorString();
            m_current = -1;
            m_terminal->setSearchResults({}, -1);
            updateLabel();
            return;
        }
        regex.optimize();
        matcher = qtRegexMatcher(regex);
    } else {
        matcher = literalMatcher(text.toStdString(), m_case->isChecked());
    }
    SearchOptions options;
    options.wholeWord = m_word->isChecked();
    auto matches = searchTerminal(m_terminal->session()->terminal(), matcher, options);

    int current = -1;
    if (!matches.empty()) {
        if (previous) {
            // Stay on the same match if it still exists, otherwise on the nearest older one.
            const auto it = std::find(matches.begin(), matches.end(), *previous);
            current = it != matches.end() ? static_cast<int>(it - matches.begin())
                                          : static_cast<int>(nextMatch(matches, previous->start, true).value_or(0));
        } else {
            current = static_cast<int>(matches.size()) - 1; // newest match, nearest the prompt
        }
    }
    m_terminal->setSearchResults(std::move(matches), current);
    select(current);
}

void SearchBar::select(int index) {
    m_current = index;
    if (m_terminal) {
        m_terminal->setSearchResults(m_terminal->searchResults(), index);
        if (index >= 0) {
            m_terminal->scrollToRow(m_terminal->searchResults()[static_cast<std::size_t>(index)].start.row);
        }
    }
    updateLabel();
}

void SearchBar::findNext() {
    const int count = matchCount();
    if (count > 0) {
        select((m_current + 1) % count);
    }
}

void SearchBar::findPrevious() {
    const int count = matchCount();
    if (count > 0) {
        select(m_current <= 0 ? count - 1 : m_current - 1);
    }
}

void SearchBar::updateLabel() {
    if (!m_error.isEmpty()) {
        m_label->setText(tr("Invalid pattern"));
        m_label->setToolTip(m_error);
        return;
    }
    m_label->setToolTip({});
    const int count = matchCount();
    if (m_edit->text().isEmpty()) {
        m_label->clear();
    } else if (count == 0) {
        m_label->setText(tr("No results"));
    } else {
        m_label->setText(tr("%1 of %2").arg(m_current + 1).arg(count));
    }
}

bool SearchBar::eventFilter(QObject* watched, QEvent* event) {
    if (watched == m_edit && event->type() == QEvent::KeyPress) {
        auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Escape) {
            hide();
            Q_EMIT closed();
            return true;
        }
        if (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) {
            if (m_debounce.isActive()) {
                m_debounce.stop();
                runSearch(false);
            }
            (key->modifiers() & Qt::ShiftModifier) ? findNext() : findPrevious();
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void SearchBar::hideEvent(QHideEvent* event) {
    // Only an explicit close clears the highlights, not hiding the whole tab.
    if (isHidden()) {
        if (m_terminal) {
            m_terminal->setSearchResults({}, -1);
        }
        m_current = -1;
    }
    QWidget::hideEvent(event);
}
