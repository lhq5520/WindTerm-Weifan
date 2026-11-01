#include "ForwardEditDialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QSpinBox>
#include <QVBoxLayout>

using wt::session::ForwardKind;
using wt::session::PortForwardSettings;

ForwardEditDialog::ForwardEditDialog(const PortForwardSettings& forward, QWidget* parent)
    : QDialog(parent),
      m_original(forward),
      m_kind(new QComboBox(this)),
      m_bindAddress(new QLineEdit(QString::fromStdString(forward.bindAddress), this)),
      m_bindPort(new QSpinBox(this)),
      m_targetHost(new QLineEdit(QString::fromStdString(forward.targetHost), this)),
      m_targetPort(new QSpinBox(this)),
      m_description(new QLineEdit(QString::fromStdString(forward.description), this)),
      m_enabled(new QCheckBox(tr("Enabled"), this)) {
    setWindowTitle(tr("Port Forward"));
    m_kind->setObjectName(QStringLiteral("forwardKind"));
    for (const ForwardKind kind : {ForwardKind::Local, ForwardKind::Remote, ForwardKind::Dynamic}) {
        m_kind->addItem(kindName(kind), static_cast<int>(kind));
    }
    m_kind->setCurrentIndex(std::max(0, m_kind->findData(static_cast<int>(forward.kind))));
    m_bindAddress->setObjectName(QStringLiteral("bindAddress"));
    m_bindPort->setObjectName(QStringLiteral("bindPort"));
    m_bindPort->setRange(0, 65535);
    m_bindPort->setValue(forward.bindPort);
    m_targetHost->setObjectName(QStringLiteral("targetHost"));
    m_targetPort->setObjectName(QStringLiteral("targetPort"));
    m_targetPort->setRange(0, 65535);
    m_targetPort->setValue(forward.targetPort);
    m_enabled->setChecked(forward.enabled);

    auto* layout = new QVBoxLayout(this);
    auto* form = new QFormLayout;
    form->addRow(tr("Type:"), m_kind);
    form->addRow(tr("Listen address:"), m_bindAddress);
    form->addRow(tr("Listen port:"), m_bindPort);
    form->addRow(tr("Destination host:"), m_targetHost);
    form->addRow(tr("Destination port:"), m_targetPort);
    form->addRow(tr("Description:"), m_description);
    form->addRow(QString(), m_enabled);
    layout->addLayout(form);
    auto* hint = new QLabel(this);
    hint->setObjectName(QStringLiteral("forwardHint"));
    hint->setWordWrap(true);
    layout->addWidget(hint);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &ForwardEditDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &ForwardEditDialog::reject);
    layout->addWidget(buttons);
    connect(m_kind, &QComboBox::currentIndexChanged, this, &ForwardEditDialog::updateVisibility);
    updateVisibility();
}

QString ForwardEditDialog::kindName(ForwardKind kind) {
    switch (kind) {
    case ForwardKind::Local: return tr("Local (-L)");
    case ForwardKind::Remote: return tr("Remote (-R)");
    case ForwardKind::Dynamic: return tr("Dynamic SOCKS (-D)");
    }
    return {};
}

void ForwardEditDialog::updateVisibility() {
    const auto kind = static_cast<ForwardKind>(m_kind->currentData().toInt());
    const bool dynamic = kind == ForwardKind::Dynamic;
    m_targetHost->setEnabled(!dynamic);
    m_targetPort->setEnabled(!dynamic);
    m_bindAddress->setPlaceholderText(kind == ForwardKind::Remote ? tr("server loopback") : QStringLiteral("127.0.0.1"));
    m_targetHost->setPlaceholderText(kind == ForwardKind::Remote ? QStringLiteral("127.0.0.1") : tr("host seen from the server"));
    QString hint;
    switch (kind) {
    case ForwardKind::Local:
        hint = tr("Connections to the local port are forwarded through the server to the destination.");
        break;
    case ForwardKind::Remote:
        hint = tr("The server listens on the port; its connections come back to the destination as seen from this "
                  "computer.");
        break;
    case ForwardKind::Dynamic:
        hint = tr("A SOCKS 4/5 proxy on the local port; programs using it connect from the server.");
        break;
    }
    if (auto* label = findChild<QLabel*>(QStringLiteral("forwardHint"))) {
        label->setText(hint);
    }
}

PortForwardSettings ForwardEditDialog::forward() const {
    PortForwardSettings forward = m_original;
    forward.kind = static_cast<ForwardKind>(m_kind->currentData().toInt());
    forward.bindAddress = m_bindAddress->text().trimmed().toStdString();
    forward.bindPort = static_cast<std::uint16_t>(m_bindPort->value());
    forward.targetHost = forward.kind == ForwardKind::Dynamic ? std::string() : m_targetHost->text().trimmed().toStdString();
    forward.targetPort = forward.kind == ForwardKind::Dynamic ? 0 : static_cast<std::uint16_t>(m_targetPort->value());
    forward.description = m_description->text().trimmed().toStdString();
    forward.enabled = m_enabled->isChecked();
    return forward;
}

QString ForwardEditDialog::problem() const {
    const PortForwardSettings f = forward();
    if (f.kind != ForwardKind::Remote && f.bindPort == 0) {
        return tr("Choose the local port to listen on.");
    }
    if (f.kind != ForwardKind::Dynamic && (f.targetHost.empty() || f.targetPort == 0)) {
        return tr("Enter the destination host and port.");
    }
    return {};
}

void ForwardEditDialog::accept() {
    if (const QString message = problem(); !message.isEmpty()) {
        QMessageBox::warning(this, windowTitle(), message);
        return;
    }
    QDialog::accept();
}
