#pragma once

#include "wt/ssh/SshTypes.h"

#include <QPointer>
#include <QString>
#include <QStringList>
#include <QWidget>

#include <functional>
#include <memory>
#include <optional>

// Questions an SSH connection asks the user, answered on the GUI thread. The default
// implementation shows dialogs; tests install scripted answers via setFactory().
class SshPrompter {
public:
    explicit SshPrompter(QWidget* parent) : m_parent(parent) {}
    virtual ~SshPrompter() = default;

    virtual wt::ssh::HostKeyDecision confirmHostKey(const QString& host, int port, const wt::ssh::HostKey& key,
                                                    wt::ssh::HostKeyCheck check);
    // Password or key passphrase; nullopt = cancelled.
    virtual std::optional<QString> askSecret(const QString& title, const QString& prompt);
    virtual std::optional<QStringList> keyboardInteractive(const QString& name, const QString& instruction,
                                                           const std::vector<wt::ssh::KeyboardPrompt>& prompts);

    using Factory = std::function<std::shared_ptr<SshPrompter>(QWidget* parent)>;
    static void setFactory(Factory factory);
    static std::shared_ptr<SshPrompter> create(QWidget* parent);

protected:
    QPointer<QWidget> m_parent;
};
