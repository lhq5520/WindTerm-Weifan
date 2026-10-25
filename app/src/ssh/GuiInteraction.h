#pragma once

#include "SshPrompter.h"

#include "wt/ssh/SshTypes.h"

#include <QString>

#include <atomic>
#include <functional>
#include <memory>

// Forwards SshInteraction calls made on a worker thread to an SshPrompter on the GUI thread and
// waits for the answer, giving up (as "cancelled") when `cancel` becomes true.
class GuiInteraction final : public wt::ssh::SshInteraction {
public:
    GuiInteraction(std::shared_ptr<SshPrompter> prompter, const std::atomic<bool>* cancel,
                   std::function<void(const QString&)> notify = {});

    wt::ssh::HostKeyDecision confirmHostKey(const std::string& host, std::uint16_t port, const wt::ssh::HostKey& key,
                                            wt::ssh::HostKeyCheck check) override;
    std::optional<std::string> askPassword(const std::string& prompt) override;
    std::optional<std::string> askPassphrase(const std::string& keyFile) override;
    std::optional<std::vector<std::string>> keyboardInteractive(const std::string& name, const std::string& instruction,
                                                                const std::vector<wt::ssh::KeyboardPrompt>& prompts) override;
    void info(const std::string& message) override;

private:
    template <typename R>
    std::optional<R> runOnGui(std::function<R(SshPrompter&)> call);

    std::shared_ptr<SshPrompter> m_prompter;
    const std::atomic<bool>* m_cancel;
    std::function<void(const QString&)> m_notify;
};
