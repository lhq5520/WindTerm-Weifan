#pragma once

#include "wt/session/SessionLog.h"
#include "wt/session/Settings.h"

#include <QByteArray>
#include <QColor>
#include <QElapsedTimer>
#include <QObject>
#include <QRegularExpression>
#include <QStringList>

#include <string_view>
#include <vector>

// A trigger rule with its regular expression compiled (PCRE2 through QRegularExpression).
struct CompiledTrigger {
    wt::session::TriggerRule rule;
    QRegularExpression regex;
    QColor foreground; // invalid: unchanged
    QColor background;
};

// Compiles the enabled rules; invalid patterns are skipped and described in `errors`.
std::vector<CompiledTrigger> compileTriggers(const std::vector<wt::session::TriggerRule>& rules,
                                             QStringList* errors = nullptr);

// Runs the notify/beep/send triggers over a session's output, line by line. The line being
// written counts too (so a "Password:" prompt can be answered); each rule fires at most once per
// line, and send rules at most a few times per second.
class TriggerRunner : public QObject {
    Q_OBJECT

public:
    using QObject::QObject;

    void setRules(std::vector<CompiledTrigger> rules);
    bool isEmpty() const { return m_rules.empty(); }
    void feed(std::string_view bytes);

Q_SIGNALS:
    void sendRequested(const QByteArray& bytes);
    void notifyRequested(const QString& title, const QString& message);
    void beepRequested();

private:
    void evaluate(const std::string& line, std::uint64_t lineNumber);

    struct State {
        std::uint64_t firedOnLine = ~std::uint64_t{0};
        QElapsedTimer window;
        int sendsInWindow = 0;
    };
    std::vector<CompiledTrigger> m_rules;
    std::vector<State> m_state;
    wt::session::PlainTextAssembler m_text;
};
