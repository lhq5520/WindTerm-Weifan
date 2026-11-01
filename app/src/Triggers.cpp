#include "Triggers.h"

#include "wt/session/Theme.h"

namespace {

constexpr int kMaxSendsPerSecond = 5;

QColor colorFrom(const std::string& text) {
    if (const auto rgb = wt::session::parseHexColor(text)) {
        return QColor::fromRgb(static_cast<QRgb>(*rgb));
    }
    return {};
}

} // namespace

std::vector<CompiledTrigger> compileTriggers(const std::vector<wt::session::TriggerRule>& rules, QStringList* errors) {
    std::vector<CompiledTrigger> compiled;
    for (const auto& rule : rules) {
        if (!rule.enabled || rule.pattern.empty()) {
            continue;
        }
        QRegularExpression regex(QString::fromStdString(rule.pattern),
                                 rule.caseSensitive ? QRegularExpression::NoPatternOption
                                                    : QRegularExpression::CaseInsensitiveOption);
        if (!regex.isValid()) {
            if (errors) {
                *errors << QObject::tr("Trigger \"%1\": %2")
                               .arg(QString::fromStdString(rule.name.empty() ? rule.pattern : rule.name),
                                    regex.errorString());
            }
            continue;
        }
        regex.optimize();
        compiled.push_back({rule, std::move(regex), colorFrom(rule.foreground), colorFrom(rule.background)});
    }
    return compiled;
}

void TriggerRunner::setRules(std::vector<CompiledTrigger> rules) {
    m_rules.clear();
    for (auto& rule : rules) {
        if (rule.rule.action != wt::session::TriggerAction::Highlight) {
            m_rules.push_back(std::move(rule)); // highlights are applied when painting
        }
    }
    m_state.assign(m_rules.size(), State{});
}

void TriggerRunner::feed(std::string_view bytes) {
    if (m_rules.empty()) {
        return;
    }
    std::vector<std::string> lines;
    const std::uint64_t first = m_text.lineNumber();
    m_text.feed(bytes, lines);
    for (std::size_t i = 0; i < lines.size(); ++i) {
        evaluate(lines[i], first + i);
    }
    if (m_text.hasCurrentLine()) {
        evaluate(m_text.currentLine(), m_text.lineNumber());
    }
}

void TriggerRunner::evaluate(const std::string& line, std::uint64_t lineNumber) {
    if (line.empty()) {
        return;
    }
    const QString text = QString::fromStdString(line);
    for (std::size_t i = 0; i < m_rules.size(); ++i) {
        State& state = m_state[i];
        if (state.firedOnLine == lineNumber) {
            continue;
        }
        const CompiledTrigger& trigger = m_rules[i];
        if (!trigger.regex.match(text).hasMatch()) {
            continue;
        }
        state.firedOnLine = lineNumber;
        switch (trigger.rule.action) {
        case wt::session::TriggerAction::Send: {
            if (!state.window.isValid() || state.window.elapsed() > 1000) {
                state.window.start();
                state.sendsInWindow = 0;
            }
            if (++state.sendsInWindow > kMaxSendsPerSecond) {
                break; // a reply that matches its own trigger must not loop forever
            }
            const std::string bytes = wt::session::unescapeTriggerText(trigger.rule.text);
            Q_EMIT sendRequested(QByteArray(bytes.data(), static_cast<qsizetype>(bytes.size())));
            break;
        }
        case wt::session::TriggerAction::Notify:
            Q_EMIT notifyRequested(QString::fromStdString(trigger.rule.name),
                                   trigger.rule.text.empty() ? text : QString::fromStdString(trigger.rule.text));
            break;
        case wt::session::TriggerAction::Beep: Q_EMIT beepRequested(); break;
        case wt::session::TriggerAction::Highlight: break;
        }
    }
}
