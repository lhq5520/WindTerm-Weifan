#include "GuiInteraction.h"

#include <QCoreApplication>
#include <QThread>

#include <chrono>
#include <future>

GuiInteraction::GuiInteraction(std::shared_ptr<SshPrompter> prompter, const std::atomic<bool>* cancel,
                               std::function<void(const QString&)> notify)
    : m_prompter(std::move(prompter)), m_cancel(cancel), m_notify(std::move(notify)) {}

template <typename R>
std::optional<R> GuiInteraction::runOnGui(std::function<R(SshPrompter&)> call) {
    if (QThread::currentThread() == QCoreApplication::instance()->thread()) {
        return call(*m_prompter);
    }
    auto promise = std::make_shared<std::promise<R>>();
    auto future = promise->get_future();
    QMetaObject::invokeMethod(
        QCoreApplication::instance(),
        [prompter = m_prompter, promise, call] { promise->set_value(call(*prompter)); }, Qt::QueuedConnection);
    while (future.wait_for(std::chrono::milliseconds(100)) != std::future_status::ready) {
        if (m_cancel && m_cancel->load()) {
            return std::nullopt; // the dialog may still be open; its answer is ignored
        }
    }
    return future.get();
}

wt::ssh::HostKeyDecision GuiInteraction::confirmHostKey(const std::string& host, std::uint16_t port,
                                                        const wt::ssh::HostKey& key, wt::ssh::HostKeyCheck check) {
    const QString qhost = QString::fromStdString(host);
    const auto decision = runOnGui<wt::ssh::HostKeyDecision>(
        [=](SshPrompter& p) { return p.confirmHostKey(qhost, port, key, check); });
    return decision.value_or(wt::ssh::HostKeyDecision::Reject);
}

std::optional<std::string> GuiInteraction::askPassword(const std::string& prompt) {
    const QString text = QString::fromStdString(prompt);
    auto answer = runOnGui<std::optional<QString>>(
        [=](SshPrompter& p) { return p.askSecret(QObject::tr("SSH Password"), text); });
    if (!answer || !*answer) {
        return std::nullopt;
    }
    return (*answer)->toStdString();
}

std::optional<std::string> GuiInteraction::askPassphrase(const std::string& keyFile) {
    const QString text = QObject::tr("Passphrase for %1:").arg(QString::fromStdString(keyFile));
    auto answer = runOnGui<std::optional<QString>>(
        [=](SshPrompter& p) { return p.askSecret(QObject::tr("Private Key Passphrase"), text); });
    if (!answer || !*answer) {
        return std::nullopt;
    }
    return (*answer)->toStdString();
}

std::optional<std::vector<std::string>> GuiInteraction::keyboardInteractive(
    const std::string& name, const std::string& instruction, const std::vector<wt::ssh::KeyboardPrompt>& prompts) {
    const QString qname = QString::fromStdString(name);
    const QString qinstruction = QString::fromStdString(instruction);
    auto answer = runOnGui<std::optional<QStringList>>(
        [=](SshPrompter& p) { return p.keyboardInteractive(qname, qinstruction, prompts); });
    if (!answer || !*answer) {
        return std::nullopt;
    }
    std::vector<std::string> result;
    for (const QString& text : **answer) {
        result.push_back(text.toStdString());
    }
    return result;
}

void GuiInteraction::info(const std::string& message) {
    if (m_notify) {
        m_notify(QString::fromStdString(message));
    }
}
