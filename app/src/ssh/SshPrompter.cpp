#include "SshPrompter.h"

#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>

namespace {

SshPrompter::Factory& factory() {
    static SshPrompter::Factory instance;
    return instance;
}

} // namespace

void SshPrompter::setFactory(Factory f) {
    factory() = std::move(f);
}

std::shared_ptr<SshPrompter> SshPrompter::create(QWidget* parent) {
    if (factory()) {
        return factory()(parent);
    }
    return std::make_shared<SshPrompter>(parent);
}

wt::ssh::HostKeyDecision SshPrompter::confirmHostKey(const QString& host, int port, const wt::ssh::HostKey& key,
                                                     wt::ssh::HostKeyCheck check) {
    const QString endpoint = port == 22 ? host : QStringLiteral("[%1]:%2").arg(host).arg(port);
    QMessageBox box(m_parent);
    box.setWindowTitle(QObject::tr("Host Key Verification"));
    const QString details = QObject::tr("%1 key fingerprint:\n%2")
                                .arg(QString::fromStdString(key.type), QString::fromStdString(key.fingerprint));
    if (check == wt::ssh::HostKeyCheck::Mismatch) {
        box.setIcon(QMessageBox::Critical);
        box.setText(QObject::tr("<b>WARNING: the host key of %1 has changed!</b>").arg(endpoint.toHtmlEscaped()));
        box.setInformativeText(QObject::tr("Someone could be eavesdropping on you (man-in-the-middle attack), or "
                                           "the host key was just changed.\n\n%1\n\nOnly continue if you are sure "
                                           "the new key is legitimate.")
                                   .arg(details));
    } else {
        box.setIcon(QMessageBox::Question);
        box.setText(QObject::tr("The authenticity of host %1 can't be established.").arg(endpoint.toHtmlEscaped()));
        box.setInformativeText(details + QObject::tr("\n\nDo you want to continue connecting?"));
    }
    QPushButton* save = box.addButton(check == wt::ssh::HostKeyCheck::Mismatch ? QObject::tr("Replace and Connect")
                                                                                : QObject::tr("Accept and Save"),
                                      QMessageBox::AcceptRole);
    QPushButton* once = box.addButton(QObject::tr("Connect Once"), QMessageBox::YesRole);
    QPushButton* reject = box.addButton(QObject::tr("Cancel"), QMessageBox::RejectRole);
    box.setDefaultButton(check == wt::ssh::HostKeyCheck::Mismatch ? reject : save);
    box.exec();
    if (box.clickedButton() == save) {
        return wt::ssh::HostKeyDecision::AcceptAndSave;
    }
    if (box.clickedButton() == once) {
        return wt::ssh::HostKeyDecision::AcceptOnce;
    }
    return wt::ssh::HostKeyDecision::Reject;
}

std::optional<QString> SshPrompter::askSecret(const QString& title, const QString& prompt) {
    bool ok = false;
    const QString text = QInputDialog::getText(m_parent, title, prompt, QLineEdit::Password, {}, &ok);
    return ok ? std::optional<QString>(text) : std::nullopt;
}

std::optional<QStringList> SshPrompter::keyboardInteractive(const QString& name, const QString& instruction,
                                                            const std::vector<wt::ssh::KeyboardPrompt>& prompts) {
    QDialog dialog(m_parent);
    dialog.setWindowTitle(name.isEmpty() ? QObject::tr("Authentication") : name);
    auto* form = new QFormLayout(&dialog);
    if (!instruction.isEmpty()) {
        auto* label = new QLabel(instruction, &dialog);
        label->setWordWrap(true);
        form->addRow(label);
    }
    QList<QLineEdit*> edits;
    for (const auto& prompt : prompts) {
        auto* edit = new QLineEdit(&dialog);
        edit->setEchoMode(prompt.echo ? QLineEdit::Normal : QLineEdit::Password);
        form->addRow(QString::fromStdString(prompt.text), edit);
        edits << edit;
    }
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    form->addRow(buttons);
    if (dialog.exec() != QDialog::Accepted) {
        return std::nullopt;
    }
    QStringList answers;
    for (QLineEdit* edit : edits) {
        answers << edit->text();
    }
    return answers;
}
