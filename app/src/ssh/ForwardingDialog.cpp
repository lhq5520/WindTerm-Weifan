#include "ForwardingDialog.h"

#include "ForwardEditDialog.h"
#include "SshBackend.h"

#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace {

QString stateText(const wt::ssh::ForwardStatus& status) {
    switch (status.state) {
    case wt::ssh::ForwardStatus::State::Starting: return QObject::tr("Starting");
    case wt::ssh::ForwardStatus::State::Active: return QObject::tr("Active");
    case wt::ssh::ForwardStatus::State::Failed:
        return QObject::tr("Failed: %1").arg(QString::fromStdString(status.message));
    case wt::ssh::ForwardStatus::State::Closed: return QObject::tr("Closed");
    }
    return {};
}

QString kindText(wt::ssh::ForwardKind kind) {
    switch (kind) {
    case wt::ssh::ForwardKind::Local: return ForwardEditDialog::kindName(wt::session::ForwardKind::Local);
    case wt::ssh::ForwardKind::Remote: return ForwardEditDialog::kindName(wt::session::ForwardKind::Remote);
    case wt::ssh::ForwardKind::Dynamic: return ForwardEditDialog::kindName(wt::session::ForwardKind::Dynamic);
    }
    return {};
}

} // namespace

ForwardingDialog::ForwardingDialog(SshBackend* backend, QWidget* parent)
    : QDialog(parent), m_backend(backend), m_list(new QTreeWidget(this)), m_summary(new QLabel(this)) {
    setWindowTitle(tr("Port Forwarding — %1").arg(backend ? backend->description() : QString()));
    auto* layout = new QVBoxLayout(this);
    m_list->setRootIsDecorated(false);
    m_list->setHeaderLabels({tr("Type"), tr("Listen"), tr("Destination"), tr("Status"), tr("Connections")});
    m_list->header()->setSectionResizeMode(3, QHeaderView::Stretch);
    layout->addWidget(m_list, 1);
    layout->addWidget(m_summary);
    auto* row = new QHBoxLayout;
    auto* add = new QPushButton(tr("Add..."), this);
    auto* remove = new QPushButton(tr("Remove"), this);
    connect(add, &QPushButton::clicked, this, [this] {
        wt::session::PortForwardSettings initial;
        initial.bindPort = 8080;
        ForwardEditDialog editor(initial, this);
        if (editor.exec() == QDialog::Accepted) {
            addForward(editor.forward());
        }
    });
    connect(remove, &QPushButton::clicked, this, &ForwardingDialog::removeSelected);
    row->addWidget(add);
    row->addWidget(remove);
    row->addStretch(1);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &ForwardingDialog::reject);
    row->addWidget(buttons);
    layout->addLayout(row);
    m_timer.setInterval(500);
    connect(&m_timer, &QTimer::timeout, this, &ForwardingDialog::refresh);
    m_timer.start();
    if (backend) {
        connect(backend, &SshBackend::forwardsChanged, this, &ForwardingDialog::refresh);
    }
    refresh();
    resize(720, 320);
}

int ForwardingDialog::addForward(const wt::session::PortForwardSettings& forward) {
    if (!m_backend) {
        return 0;
    }
    const int id = m_backend->addForward(toSshForward(forward));
    refresh();
    return id;
}

void ForwardingDialog::removeSelected() {
    if (!m_backend) {
        return;
    }
    if (QTreeWidgetItem* item = m_list->currentItem()) {
        m_backend->removeForward(item->data(0, Qt::UserRole).toInt());
        refresh();
    }
}

void ForwardingDialog::refresh() {
    const int selected = m_list->currentItem() ? m_list->currentItem()->data(0, Qt::UserRole).toInt() : 0;
    m_list->clear();
    if (!m_backend) {
        m_summary->setText(tr("The session is closed."));
        return;
    }
    for (const auto& status : m_backend->forwards()) {
        const auto& f = status.forward;
        const QString listen = QStringLiteral("%1:%2")
                                   .arg(f.bindAddress.empty() ? (f.kind == wt::ssh::ForwardKind::Remote
                                                                     ? QStringLiteral("localhost")
                                                                     : QStringLiteral("127.0.0.1"))
                                                              : QString::fromStdString(f.bindAddress))
                                   .arg(status.boundPort ? status.boundPort : f.bindPort);
        const QString target = f.kind == wt::ssh::ForwardKind::Dynamic
                                   ? tr("(SOCKS)")
                                   : QStringLiteral("%1:%2").arg(QString::fromStdString(f.targetHost)).arg(f.targetPort);
        auto* item = new QTreeWidgetItem(
            m_list, {kindText(f.kind), listen, target, stateText(status),
                     QStringLiteral("%1 / %2").arg(status.openConnections).arg(status.totalConnections)});
        item->setData(0, Qt::UserRole, status.id);
        if (status.id == selected) {
            m_list->setCurrentItem(item);
        }
    }
    QStringList extras;
    if (m_backend->agentForwardingActive()) {
        extras << tr("agent forwarding on");
    }
    if (m_backend->x11ForwardingActive()) {
        extras << tr("X11 forwarding on");
    }
    m_summary->setText(m_backend->state() == SshBackend::State::Connected
                           ? (extras.isEmpty() ? tr("Connected.") : tr("Connected; %1.").arg(extras.join(QStringLiteral(", "))))
                           : tr("Not connected: forwards start when the session reconnects."));
}
