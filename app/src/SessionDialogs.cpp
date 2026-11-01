#include "SessionDialogs.h"

#include "AppContext.h"
#include "ForwardEditDialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIntValidator>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTabWidget>
#include <QTreeWidget>
#include <QVBoxLayout>

using namespace wt::session;

namespace {

// Splits a command line on whitespace, honouring "double" and 'single' quotes.
QStringList splitArguments(const QString& text) {
    QStringList args;
    QString current;
    QChar quote;
    bool hasCurrent = false;
    for (const QChar c : text) {
        if (!quote.isNull()) {
            if (c == quote) {
                quote = QChar();
            } else {
                current += c;
            }
        } else if (c == QLatin1Char('"') || c == QLatin1Char('\'')) {
            quote = c;
            hasCurrent = true;
        } else if (c.isSpace()) {
            if (hasCurrent) {
                args << current;
                current.clear();
                hasCurrent = false;
            }
        } else {
            current += c;
            hasCurrent = true;
        }
    }
    if (hasCurrent) {
        args << current;
    }
    return args;
}

QString joinArguments(const std::vector<std::string>& args) {
    QStringList parts;
    for (const auto& arg : args) {
        QString text = QString::fromStdString(arg);
        if (text.isEmpty() || text.contains(QLatin1Char(' ')) || text.contains(QLatin1Char('"'))) {
            text = QLatin1Char('\'') + text + QLatin1Char('\'');
        }
        parts << text;
    }
    return parts.join(QLatin1Char(' '));
}

QSpinBox* spinBox(int min, int max, int value, QWidget* parent) {
    auto* box = new QSpinBox(parent);
    box->setRange(min, max);
    box->setValue(value);
    return box;
}

} // namespace

// ---------------------------------------------------------------------------------------------
// SessionEditDialog

SessionEditDialog::SessionEditDialog(AppContext* context, const SessionProfile& profile, QWidget* parent)
    : QDialog(parent), m_context(context), m_original(profile) {
    setWindowTitle(profile.id.empty() ? tr("New Session") : tr("Edit Session"));
    auto* layout = new QVBoxLayout(this);
    auto* pages = new QTabWidget(this);
    pages->addTab(createGeneralPage(), tr("General"));
    pages->addTab(createProxyPage(), tr("Proxy && Reconnect"));
    pages->addTab(createForwardingPage(), tr("Forwarding"));
    layout->addWidget(pages);
    if (profile.id.empty()) {
        m_saveToList = new QCheckBox(tr("Save in the session list"), this);
        m_saveToList->setObjectName(QStringLiteral("saveToList"));
        m_saveToList->setChecked(true);
        layout->addWidget(m_saveToList);
    }
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &SessionEditDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &SessionEditDialog::reject);
    layout->addWidget(buttons);
    updateVisibility();
    resize(560, 560);
}

QWidget* SessionEditDialog::createGeneralPage() {
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);
    auto* form = new QFormLayout;
    m_name = new QLineEdit(QString::fromStdString(m_original.name), page);
    m_name->setObjectName(QStringLiteral("name"));
    m_group = new QComboBox(page);
    m_group->setEditable(true);
    m_group->addItem(QString());
    if (m_context) {
        for (const auto& group : m_context->sessions().groups()) {
            m_group->addItem(QString::fromStdString(group));
        }
    }
    m_group->setCurrentText(QString::fromStdString(m_original.group));
    m_kind = new QComboBox(page);
    m_kind->setObjectName(QStringLiteral("kind"));
    m_kind->addItem(tr("SSH"), static_cast<int>(SessionKind::Ssh));
    m_kind->addItem(tr("Telnet"), static_cast<int>(SessionKind::Telnet));
    m_kind->addItem(tr("Serial port"), static_cast<int>(SessionKind::Serial));
    m_kind->addItem(tr("Local shell"), static_cast<int>(SessionKind::Local));
    m_kind->setCurrentIndex(m_kind->findData(static_cast<int>(m_original.kind)));
    m_theme = new QComboBox(page);
    m_theme->addItem(tr("(application theme)"), QString());
    for (const auto& theme : builtinThemes()) {
        m_theme->addItem(QString::fromStdString(theme.name), QString::fromStdString(theme.name));
    }
    if (m_context) {
        for (const auto& theme : m_context->settings().customThemes) {
            m_theme->addItem(QString::fromStdString(theme.name), QString::fromStdString(theme.name));
        }
    }
    m_theme->setCurrentIndex(std::max(0, m_theme->findData(QString::fromStdString(m_original.theme))));
    form->addRow(tr("Name:"), m_name);
    form->addRow(tr("Folder:"), m_group);
    form->addRow(tr("Type:"), m_kind);
    form->addRow(tr("Theme:"), m_theme);
    layout->addLayout(form);

    m_kindPages = new QStackedWidget(page);
    m_kindPages->addWidget(createSshPage());
    m_kindPages->addWidget(createLocalPage());
    m_kindPages->addWidget(createTelnetPage());
    m_kindPages->addWidget(createSerialPage());
    layout->addWidget(m_kindPages);
    m_notes = new QPlainTextEdit(QString::fromStdString(m_original.notes), page);
    m_notes->setPlaceholderText(tr("Notes"));
    m_notes->setMaximumHeight(70);
    layout->addWidget(m_notes);
    connect(m_kind, &QComboBox::currentIndexChanged, this, &SessionEditDialog::updateVisibility);
    return page;
}

QWidget* SessionEditDialog::createSshPage() {
    auto* page = new QWidget(this);
    auto* form = new QFormLayout(page);
    form->setContentsMargins(0, 0, 0, 0);
    const SshSettings& ssh = m_original.ssh;
    m_host = new QLineEdit(QString::fromStdString(ssh.host), page);
    m_host->setObjectName(QStringLiteral("host"));
    m_port = spinBox(1, 65535, ssh.port == 0 ? 22 : ssh.port, page);
    m_port->setObjectName(QStringLiteral("port"));
    auto* hostRow = new QHBoxLayout;
    hostRow->addWidget(m_host, 1);
    hostRow->addWidget(new QLabel(tr("Port:"), page));
    hostRow->addWidget(m_port);
    m_user = new QLineEdit(QString::fromStdString(ssh.username), page);
    m_user->setObjectName(QStringLiteral("user"));
    m_auth = new QComboBox(page);
    m_auth->setObjectName(QStringLiteral("auth"));
    m_auth->addItem(tr("Automatic (agent, keys, password)"), static_cast<int>(AuthMethod::Auto));
    m_auth->addItem(tr("Password"), static_cast<int>(AuthMethod::Password));
    m_auth->addItem(tr("Public key"), static_cast<int>(AuthMethod::PublicKey));
    m_auth->addItem(tr("SSH agent"), static_cast<int>(AuthMethod::Agent));
    m_auth->addItem(tr("Keyboard-interactive"), static_cast<int>(AuthMethod::KeyboardInteractive));
    m_auth->setCurrentIndex(m_auth->findData(static_cast<int>(ssh.auth)));
    m_identity = new QLineEdit(QString::fromStdString(ssh.identityFile), page);
    m_identity->setObjectName(QStringLiteral("identity"));
    m_identity->setPlaceholderText(QStringLiteral("~/.ssh/id_ed25519"));
    auto* browse = new QPushButton(tr("Browse..."), page);
    connect(browse, &QPushButton::clicked, this, [this] {
        const QString file = QFileDialog::getOpenFileName(this, tr("Private key"), QDir::homePath() + QStringLiteral("/.ssh"));
        if (!file.isEmpty()) {
            m_identity->setText(file);
        }
    });
    auto* keyRow = new QHBoxLayout;
    keyRow->addWidget(m_identity, 1);
    keyRow->addWidget(browse);
    m_certificate = new QLineEdit(QString::fromStdString(ssh.certificateFile), page);
    m_certificate->setObjectName(QStringLiteral("certificate"));
    m_certificate->setPlaceholderText(tr("Optional: <key>-cert.pub is used when it exists"));
    m_secret = new QLineEdit(page);
    m_secret->setObjectName(QStringLiteral("secret"));
    m_secret->setEchoMode(QLineEdit::Password);
    m_secret->setPlaceholderText(ssh.credentialId.empty() ? tr("Ask when connecting")
                                                          : tr("Stored in the credential vault"));
    m_storeSecret = new QCheckBox(
        tr("Remember it in the %1").arg(m_context ? m_context->credentialStoreName() : tr("encrypted credential vault")),
        page);
    m_storeSecret->setObjectName(QStringLiteral("storeSecret"));
    m_storeSecret->setChecked(!ssh.credentialId.empty());
    m_keepAlive = spinBox(0, 3600, ssh.keepAliveSeconds, page);
    m_keepAlive->setSuffix(tr(" s"));
    m_keepAlive->setSpecialValueText(tr("Off"));
    m_timeout = spinBox(1, 600, ssh.connectTimeoutSeconds, page);
    m_timeout->setSuffix(tr(" s"));
    m_command = new QLineEdit(QString::fromStdString(ssh.remoteCommand), page);
    m_command->setPlaceholderText(tr("Interactive shell"));

    form->addRow(tr("Host:"), hostRow);
    form->addRow(tr("User:"), m_user);
    form->addRow(tr("Authentication:"), m_auth);
    form->addRow(tr("Private key:"), keyRow);
    form->addRow(tr("Certificate:"), m_certificate);
    form->addRow(tr("Password / passphrase:"), m_secret);
    form->addRow(QString(), m_storeSecret);
    form->addRow(tr("Keep-alive interval:"), m_keepAlive);
    form->addRow(tr("Connect timeout:"), m_timeout);
    form->addRow(tr("Remote command:"), m_command);
    return page;
}

QWidget* SessionEditDialog::createLocalPage() {
    auto* page = new QWidget(this);
    auto* form = new QFormLayout(page);
    form->setContentsMargins(0, 0, 0, 0);
    const LocalSettings& local = m_original.local;
    m_program = new QLineEdit(QString::fromStdString(local.program), page);
    m_program->setObjectName(QStringLiteral("program"));
    m_program->setPlaceholderText(tr("Login shell"));
    m_arguments = new QLineEdit(joinArguments(local.arguments), page);
    m_workingDirectory = new QLineEdit(QString::fromStdString(local.workingDirectory), page);
    m_workingDirectory->setPlaceholderText(QDir::homePath());
    m_loginShell = new QCheckBox(tr("Run as a login shell"), page);
    m_loginShell->setChecked(local.loginShell);
    form->addRow(tr("Program:"), m_program);
    form->addRow(tr("Arguments:"), m_arguments);
    form->addRow(tr("Working directory:"), m_workingDirectory);
    form->addRow(QString(), m_loginShell);
    return page;
}

QWidget* SessionEditDialog::createTelnetPage() {
    auto* page = new QWidget(this);
    auto* form = new QFormLayout(page);
    form->setContentsMargins(0, 0, 0, 0);
    const TelnetSettings& telnet = m_original.telnet;
    m_telnetHost = new QLineEdit(QString::fromStdString(telnet.host), page);
    m_telnetHost->setObjectName(QStringLiteral("telnetHost"));
    m_telnetPort = spinBox(1, 65535, telnet.port == 0 ? 23 : telnet.port, page);
    m_telnetPort->setObjectName(QStringLiteral("telnetPort"));
    auto* hostRow = new QHBoxLayout;
    hostRow->addWidget(m_telnetHost, 1);
    hostRow->addWidget(new QLabel(tr("Port:"), page));
    hostRow->addWidget(m_telnetPort);
    m_terminalType = new QComboBox(page);
    m_terminalType->setObjectName(QStringLiteral("terminalType"));
    m_terminalType->setEditable(true);
    m_terminalType->addItems({QStringLiteral("xterm-256color"), QStringLiteral("xterm"), QStringLiteral("vt220"),
                              QStringLiteral("vt100"), QStringLiteral("ansi"), QStringLiteral("linux")});
    m_terminalType->setCurrentText(telnet.terminalType.empty() ? QStringLiteral("xterm-256color")
                                                               : QString::fromStdString(telnet.terminalType));
    m_telnetEcho = new QComboBox(page);
    m_telnetEcho->setObjectName(QStringLiteral("telnetEcho"));
    m_telnetEcho->addItem(tr("Automatic (when the server does not echo)"), static_cast<int>(EchoMode::Auto));
    m_telnetEcho->addItem(tr("Always"), static_cast<int>(EchoMode::On));
    m_telnetEcho->addItem(tr("Never"), static_cast<int>(EchoMode::Off));
    m_telnetEcho->setCurrentIndex(std::max(0, m_telnetEcho->findData(static_cast<int>(telnet.localEcho))));
    m_telnetCrlf = new QCheckBox(tr("Send Enter as CR LF (otherwise CR NUL)"), page);
    m_telnetCrlf->setChecked(telnet.crlf);
    m_telnetTimeout = spinBox(1, 600, telnet.connectTimeoutSeconds, page);
    m_telnetTimeout->setSuffix(tr(" s"));
    form->addRow(tr("Host:"), hostRow);
    form->addRow(tr("Terminal type:"), m_terminalType);
    form->addRow(tr("Local echo:"), m_telnetEcho);
    form->addRow(QString(), m_telnetCrlf);
    form->addRow(tr("Connect timeout:"), m_telnetTimeout);
    return page;
}

QWidget* SessionEditDialog::createSerialPage() {
    auto* page = new QWidget(this);
    auto* form = new QFormLayout(page);
    form->setContentsMargins(0, 0, 0, 0);
    const SerialSettings& serial = m_original.serial;
    m_serialDevice = new QComboBox(page);
    m_serialDevice->setObjectName(QStringLiteral("serialDevice"));
    m_serialDevice->setEditable(true);
    m_serialDevice->setMinimumContentsLength(18);
    auto* refresh = new QPushButton(tr("Refresh"), page);
    connect(refresh, &QPushButton::clicked, this, &SessionEditDialog::refreshSerialPorts);
    auto* deviceRow = new QHBoxLayout;
    deviceRow->addWidget(m_serialDevice, 1);
    deviceRow->addWidget(refresh);
    m_serialDevice->setCurrentText(QString::fromStdString(serial.line.device));
    refreshSerialPorts();
    if (m_original.kind == SessionKind::Serial && serial.line.device.empty() && m_serialDevice->count() > 0) {
        m_serialDevice->setCurrentIndex(0); // a new serial session: suggest the first port
    }

    m_baudRate = new QComboBox(page);
    m_baudRate->setObjectName(QStringLiteral("baudRate"));
    m_baudRate->setEditable(true);
    m_baudRate->setValidator(new QIntValidator(50, 16000000, m_baudRate));
    for (const int rate : wt::platform::standardBaudRates()) {
        m_baudRate->addItem(QString::number(rate));
    }
    m_baudRate->setCurrentText(QString::number(serial.line.baudRate));
    m_dataBits = new QComboBox(page);
    m_dataBits->setObjectName(QStringLiteral("dataBits"));
    for (const int bits : {8, 7, 6, 5}) {
        m_dataBits->addItem(QString::number(bits), bits);
    }
    m_dataBits->setCurrentIndex(std::max(0, m_dataBits->findData(serial.line.dataBits)));
    m_parity = new QComboBox(page);
    m_parity->setObjectName(QStringLiteral("parity"));
    m_parity->addItem(tr("None"), static_cast<int>(wt::platform::Parity::None));
    m_parity->addItem(tr("Odd"), static_cast<int>(wt::platform::Parity::Odd));
    m_parity->addItem(tr("Even"), static_cast<int>(wt::platform::Parity::Even));
    m_parity->addItem(tr("Mark"), static_cast<int>(wt::platform::Parity::Mark));
    m_parity->addItem(tr("Space"), static_cast<int>(wt::platform::Parity::Space));
    m_parity->setCurrentIndex(std::max(0, m_parity->findData(static_cast<int>(serial.line.parity))));
    m_stopBits = new QComboBox(page);
    m_stopBits->setObjectName(QStringLiteral("stopBits"));
    m_stopBits->addItem(QStringLiteral("1"), static_cast<int>(wt::platform::StopBits::One));
    m_stopBits->addItem(QStringLiteral("1.5"), static_cast<int>(wt::platform::StopBits::OneAndHalf));
    m_stopBits->addItem(QStringLiteral("2"), static_cast<int>(wt::platform::StopBits::Two));
    m_stopBits->setCurrentIndex(std::max(0, m_stopBits->findData(static_cast<int>(serial.line.stopBits))));
    m_flowControl = new QComboBox(page);
    m_flowControl->setObjectName(QStringLiteral("flowControl"));
    m_flowControl->addItem(tr("None"), static_cast<int>(wt::platform::FlowControl::None));
    m_flowControl->addItem(tr("XON/XOFF (software)"), static_cast<int>(wt::platform::FlowControl::XonXoff));
    m_flowControl->addItem(tr("RTS/CTS (hardware)"), static_cast<int>(wt::platform::FlowControl::RtsCts));
    m_flowControl->addItem(tr("DSR/DTR (hardware)"), static_cast<int>(wt::platform::FlowControl::DsrDtr));
    m_flowControl->setCurrentIndex(
        std::max(0, m_flowControl->findData(static_cast<int>(serial.line.flowControl))));
    m_serialEcho = new QComboBox(page);
    m_serialEcho->setObjectName(QStringLiteral("serialEcho"));
    m_serialEcho->addItem(tr("Off (the device echoes)"), static_cast<int>(EchoMode::Off));
    m_serialEcho->addItem(tr("On"), static_cast<int>(EchoMode::On));
    m_serialEcho->setCurrentIndex(std::max(0, m_serialEcho->findData(static_cast<int>(serial.localEcho))));
    m_serialNewline = new QComboBox(page);
    m_serialNewline->setObjectName(QStringLiteral("serialNewline"));
    m_serialNewline->addItem(tr("CR"), static_cast<int>(NewlineMode::Cr));
    m_serialNewline->addItem(tr("LF"), static_cast<int>(NewlineMode::Lf));
    m_serialNewline->addItem(tr("CR LF"), static_cast<int>(NewlineMode::CrLf));
    m_serialNewline->setCurrentIndex(std::max(0, m_serialNewline->findData(static_cast<int>(serial.newline))));

    auto* framing = new QHBoxLayout;
    framing->addWidget(m_dataBits);
    framing->addWidget(new QLabel(tr("Parity:"), page));
    framing->addWidget(m_parity);
    framing->addWidget(new QLabel(tr("Stop bits:"), page));
    framing->addWidget(m_stopBits);
    form->addRow(tr("Device:"), deviceRow);
    form->addRow(tr("Baud rate:"), m_baudRate);
    form->addRow(tr("Data bits:"), framing);
    form->addRow(tr("Flow control:"), m_flowControl);
    form->addRow(tr("Local echo:"), m_serialEcho);
    form->addRow(tr("Enter sends:"), m_serialNewline);
    return page;
}

QWidget* SessionEditDialog::createForwardingPage() {
    m_forwardingPage = new QWidget(this);
    auto* layout = new QVBoxLayout(m_forwardingPage);
    m_forwards = m_original.ssh.forwards;
    m_forwardList = new QTreeWidget(m_forwardingPage);
    m_forwardList->setObjectName(QStringLiteral("forwardList"));
    m_forwardList->setRootIsDecorated(false);
    m_forwardList->setHeaderLabels({tr("Type"), tr("Listen"), tr("Destination"), tr("Description")});
    connect(m_forwardList, &QTreeWidget::itemDoubleClicked, this,
            [this](QTreeWidgetItem* item) { editForward(m_forwardList->indexOfTopLevelItem(item)); });
    connect(m_forwardList, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem* item, int column) {
        const int index = m_forwardList->indexOfTopLevelItem(item);
        if (column == 0 && index >= 0 && index < static_cast<int>(m_forwards.size())) {
            m_forwards[static_cast<std::size_t>(index)].enabled = item->checkState(0) == Qt::Checked;
        }
    });
    layout->addWidget(m_forwardList, 1);
    auto* buttons = new QHBoxLayout;
    auto* add = new QPushButton(tr("Add..."), m_forwardingPage);
    add->setObjectName(QStringLiteral("addForward"));
    auto* edit = new QPushButton(tr("Edit..."), m_forwardingPage);
    auto* remove = new QPushButton(tr("Remove"), m_forwardingPage);
    remove->setObjectName(QStringLiteral("removeForward"));
    connect(add, &QPushButton::clicked, this, [this] { editForward(-1); });
    connect(edit, &QPushButton::clicked, this,
            [this] { editForward(m_forwardList->indexOfTopLevelItem(m_forwardList->currentItem())); });
    connect(remove, &QPushButton::clicked, this, [this] {
        const int index = m_forwardList->indexOfTopLevelItem(m_forwardList->currentItem());
        if (index >= 0 && index < static_cast<int>(m_forwards.size())) {
            m_forwards.erase(m_forwards.begin() + index);
            reloadForwards();
        }
    });
    buttons->addWidget(add);
    buttons->addWidget(edit);
    buttons->addWidget(remove);
    buttons->addStretch(1);
    layout->addLayout(buttons);

    m_forwardAgent = new QCheckBox(tr("Forward the SSH agent (only for servers you trust)"), m_forwardingPage);
    m_forwardAgent->setObjectName(QStringLiteral("forwardAgent"));
    m_forwardAgent->setChecked(m_original.ssh.forwardAgent);
    m_forwardX11 = new QCheckBox(tr("Forward X11 connections"), m_forwardingPage);
    m_forwardX11->setObjectName(QStringLiteral("forwardX11"));
    m_forwardX11->setChecked(m_original.ssh.forwardX11);
    m_x11Display = new QLineEdit(QString::fromStdString(m_original.ssh.x11Display), m_forwardingPage);
    m_x11Display->setObjectName(QStringLiteral("x11Display"));
    m_x11Display->setPlaceholderText(tr("$DISPLAY"));
    auto* x11Row = new QHBoxLayout;
    x11Row->addWidget(m_forwardX11);
    x11Row->addWidget(new QLabel(tr("Display:"), m_forwardingPage));
    x11Row->addWidget(m_x11Display, 1);
    layout->addWidget(m_forwardAgent);
    layout->addLayout(x11Row);
    reloadForwards();
    return m_forwardingPage;
}

void SessionEditDialog::reloadForwards() {
    const QSignalBlocker blocker(m_forwardList);
    m_forwardList->clear();
    for (const auto& forward : m_forwards) {
        const QString listen = QString::fromStdString(
            (forward.bindAddress.empty() ? std::string() : forward.bindAddress + ":") + std::to_string(forward.bindPort));
        const QString target = forward.kind == ForwardKind::Dynamic
                                   ? tr("(SOCKS)")
                                   : QString::fromStdString(forward.targetHost + ":" + std::to_string(forward.targetPort));
        auto* item = new QTreeWidgetItem(m_forwardList, {ForwardEditDialog::kindName(forward.kind), listen, target,
                                                         QString::fromStdString(forward.description)});
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(0, forward.enabled ? Qt::Checked : Qt::Unchecked);
    }
}

void SessionEditDialog::editForward(int index) {
    const bool adding = index < 0 || index >= static_cast<int>(m_forwards.size());
    PortForwardSettings initial;
    if (!adding) {
        initial = m_forwards[static_cast<std::size_t>(index)];
    }
    ForwardEditDialog dialog(initial, this);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    if (adding) {
        m_forwards.push_back(dialog.forward());
    } else {
        m_forwards[static_cast<std::size_t>(index)] = dialog.forward();
    }
    reloadForwards();
}

void SessionEditDialog::refreshSerialPorts() {
    if (!m_serialDevice) {
        return;
    }
    const QString current = m_serialDevice->currentText();
    m_serialDevice->clear();
    for (const auto& port : wt::platform::availableSerialPorts()) {
        const QString device = QString::fromStdString(port.device);
        m_serialDevice->addItem(device, device);
        if (!port.description.empty()) {
            m_serialDevice->setItemData(m_serialDevice->count() - 1, QString::fromStdString(port.description),
                                        Qt::ToolTipRole);
        }
    }
    m_serialDevice->setCurrentText(current);
}

QWidget* SessionEditDialog::createProxyPage() {
    auto* page = new QWidget(this);
    auto* form = new QFormLayout(page);
    const ProxySettings& proxy = m_original.kind == SessionKind::Telnet ? m_original.telnet.proxy : m_original.ssh.proxy;
    m_proxyKind = new QComboBox(page);
    m_proxyKind->setObjectName(QStringLiteral("proxyKind"));
    m_proxyKind->addItem(tr("Direct connection"), static_cast<int>(ProxyKind::None));
    m_proxyKind->addItem(tr("SOCKS5 proxy"), static_cast<int>(ProxyKind::Socks5));
    m_proxyKind->addItem(tr("HTTP proxy (CONNECT)"), static_cast<int>(ProxyKind::HttpConnect));
    m_proxyKind->addItem(tr("Jump host (SSH)"), static_cast<int>(ProxyKind::JumpHost));
    m_proxyKind->setCurrentIndex(m_proxyKind->findData(static_cast<int>(proxy.kind)));
    m_proxyHost = new QLineEdit(QString::fromStdString(proxy.host), page);
    m_proxyPort = spinBox(0, 65535, proxy.port, page);
    m_proxyUser = new QLineEdit(QString::fromStdString(proxy.username), page);
    m_jump = new QComboBox(page);
    m_jump->setObjectName(QStringLiteral("jumpHost"));
    m_jump->addItem(tr("(choose a saved SSH session)"), QString());
    if (m_context) {
        for (const auto& candidate : m_context->sessions().profiles()) {
            if (candidate.kind == SessionKind::Ssh && candidate.id != m_original.id) {
                m_jump->addItem(QString::fromStdString(candidate.name + " (" + candidate.target() + ")"),
                                QString::fromStdString(candidate.id));
            }
        }
    }
    m_jump->setCurrentIndex(std::max(0, m_jump->findData(QString::fromStdString(proxy.jumpProfileId))));
    const ReconnectSettings& reconnect = m_original.kind == SessionKind::Telnet   ? m_original.telnet.reconnect
                                         : m_original.kind == SessionKind::Serial ? m_original.serial.reconnect
                                                                                  : m_original.ssh.reconnect;
    m_reconnect = new QCheckBox(tr("Reconnect automatically when the connection drops"), page);
    m_reconnect->setObjectName(QStringLiteral("reconnect"));
    m_reconnect->setChecked(reconnect.enabled);
    m_reconnectAttempts = spinBox(1, 1000, std::max(1, reconnect.maxAttempts), page);

    form->addRow(tr("Connect via:"), m_proxyKind);
    form->addRow(tr("Proxy host:"), m_proxyHost);
    form->addRow(tr("Proxy port:"), m_proxyPort);
    form->addRow(tr("Proxy user:"), m_proxyUser);
    form->addRow(tr("Jump host:"), m_jump);
    form->addRow(m_reconnect);
    form->addRow(tr("Reconnect attempts:"), m_reconnectAttempts);
    connect(m_proxyKind, &QComboBox::currentIndexChanged, this, &SessionEditDialog::updateVisibility);
    return page;
}

void SessionEditDialog::updateVisibility() {
    const auto kind = static_cast<SessionKind>(m_kind->currentData().toInt());
    switch (kind) {
    case SessionKind::Ssh: m_kindPages->setCurrentIndex(0); break;
    case SessionKind::Local: m_kindPages->setCurrentIndex(1); break;
    case SessionKind::Telnet: m_kindPages->setCurrentIndex(2); break;
    case SessionKind::Serial: m_kindPages->setCurrentIndex(3); break;
    }
    const auto proxy = static_cast<ProxyKind>(m_proxyKind->currentData().toInt());
    const bool ssh = kind == SessionKind::Ssh;
    const bool proxied = ssh || kind == SessionKind::Telnet;
    const bool network = proxied && (proxy == ProxyKind::Socks5 || proxy == ProxyKind::HttpConnect);
    m_proxyKind->setEnabled(proxied);
    m_proxyHost->setEnabled(network);
    m_proxyPort->setEnabled(network);
    m_proxyUser->setEnabled(network);
    m_jump->setEnabled(ssh && proxy == ProxyKind::JumpHost);
    const bool remote = kind != SessionKind::Local;
    m_reconnect->setEnabled(remote);
    m_reconnectAttempts->setEnabled(remote);
    m_forwardingPage->setEnabled(ssh);
}

SessionProfile SessionEditDialog::profile() const {
    SessionProfile p = m_original;
    p.name = m_name->text().trimmed().toStdString();
    p.group = m_group->currentText().trimmed().toStdString();
    p.kind = static_cast<SessionKind>(m_kind->currentData().toInt());
    p.theme = m_theme->currentData().toString().toStdString();
    p.notes = m_notes->toPlainText().toStdString();
    p.local.program = m_program->text().trimmed().toStdString();
    p.local.arguments.clear();
    for (const QString& arg : splitArguments(m_arguments->text())) {
        p.local.arguments.push_back(arg.toStdString());
    }
    p.local.workingDirectory = m_workingDirectory->text().trimmed().toStdString();
    p.local.loginShell = m_loginShell->isChecked();
    p.ssh.host = m_host->text().trimmed().toStdString();
    p.ssh.port = static_cast<std::uint16_t>(m_port->value());
    p.ssh.username = m_user->text().trimmed().toStdString();
    p.ssh.auth = static_cast<AuthMethod>(m_auth->currentData().toInt());
    p.ssh.identityFile = m_identity->text().trimmed().toStdString();
    p.ssh.certificateFile = m_certificate->text().trimmed().toStdString();
    if (!m_storeSecret->isChecked()) {
        p.ssh.credentialId.clear();
    } else if (!m_credentialId.empty()) {
        p.ssh.credentialId = m_credentialId;
    }
    p.ssh.keepAliveSeconds = m_keepAlive->value();
    p.ssh.connectTimeoutSeconds = m_timeout->value();
    p.ssh.remoteCommand = m_command->text().toStdString();
    p.ssh.forwards = m_forwards;
    p.ssh.forwardAgent = m_forwardAgent->isChecked();
    p.ssh.forwardX11 = m_forwardX11->isChecked();
    p.ssh.x11Display = m_x11Display->text().trimmed().toStdString();

    if (p.kind == SessionKind::Telnet) {
        p.telnet.host = m_telnetHost->text().trimmed().toStdString();
        p.telnet.port = static_cast<std::uint16_t>(m_telnetPort->value());
        p.telnet.terminalType = m_terminalType->currentText().trimmed().toStdString();
        if (p.telnet.terminalType == "xterm-256color") {
            p.telnet.terminalType.clear(); // the default
        }
        p.telnet.localEcho = static_cast<EchoMode>(m_telnetEcho->currentData().toInt());
        p.telnet.crlf = m_telnetCrlf->isChecked();
        p.telnet.connectTimeoutSeconds = m_telnetTimeout->value();
    }
    if (p.kind == SessionKind::Serial) {
        p.serial.line.device = m_serialDevice->currentText().trimmed().toStdString();
        p.serial.line.baudRate = m_baudRate->currentText().toInt();
        p.serial.line.dataBits = m_dataBits->currentData().toInt();
        p.serial.line.parity = static_cast<wt::platform::Parity>(m_parity->currentData().toInt());
        p.serial.line.stopBits = static_cast<wt::platform::StopBits>(m_stopBits->currentData().toInt());
        p.serial.line.flowControl = static_cast<wt::platform::FlowControl>(m_flowControl->currentData().toInt());
        p.serial.localEcho = static_cast<EchoMode>(m_serialEcho->currentData().toInt());
        p.serial.newline = static_cast<NewlineMode>(m_serialNewline->currentData().toInt());
    }

    // The proxy and reconnect page applies to the selected kind.
    ProxySettings proxy;
    proxy.kind = static_cast<ProxyKind>(m_proxyKind->currentData().toInt());
    proxy.host = m_proxyHost->text().trimmed().toStdString();
    proxy.port = static_cast<std::uint16_t>(m_proxyPort->value());
    proxy.username = m_proxyUser->text().trimmed().toStdString();
    proxy.jumpProfileId = m_jump->currentData().toString().toStdString();
    ReconnectSettings* reconnect = &p.ssh.reconnect;
    if (p.kind == SessionKind::Telnet) {
        proxy.credentialId = p.telnet.proxy.credentialId;
        if (proxy.kind == ProxyKind::JumpHost) {
            proxy.kind = ProxyKind::None; // not available for Telnet
        }
        proxy.jumpProfileId.clear();
        p.telnet.proxy = proxy;
        reconnect = &p.telnet.reconnect;
    } else if (p.kind == SessionKind::Serial) {
        reconnect = &p.serial.reconnect;
    } else if (p.kind == SessionKind::Ssh) {
        proxy.credentialId = p.ssh.proxy.credentialId;
        p.ssh.proxy = proxy;
    }
    if (p.kind != SessionKind::Local) {
        reconnect->enabled = m_reconnect->isChecked();
        reconnect->maxAttempts = m_reconnectAttempts->value();
    }
    if (p.name.empty()) {
        switch (p.kind) {
        case SessionKind::Ssh: p.name = p.target(); break;
        case SessionKind::Telnet: p.name = p.telnet.host; break;
        case SessionKind::Serial: p.name = p.serial.line.device; break;
        case SessionKind::Local: p.name = p.local.program.empty() ? "Shell" : p.local.program; break;
        }
    }
    return p;
}

bool SessionEditDialog::saveRequested() const {
    return !m_saveToList || m_saveToList->isChecked();
}

QString SessionEditDialog::secret() const {
    return m_secret->text();
}

bool SessionEditDialog::storeSecret() const {
    return m_storeSecret->isChecked();
}

QStringList SessionEditDialog::problems() const {
    QStringList list;
    if (!m_context) {
        return list;
    }
    for (const auto& problem : m_context->sessions().validate(profile())) {
        list << QString::fromStdString(problem);
    }
    return list;
}

void SessionEditDialog::accept() {
    if (const QStringList list = problems(); !list.isEmpty()) {
        QMessageBox::warning(this, windowTitle(), list.join(QLatin1Char('\n')));
        return;
    }
    if (m_context && storeSecret() && !secret().isEmpty()) {
        // Keep the password/passphrase in the encrypted vault; the profile only references it.
        const SessionProfile p = profile();
        const std::string label = p.ssh.username + "@" + p.ssh.host;
        const std::string id = m_context->storeCredential(m_original.ssh.credentialId, secret().toStdString(), label, this);
        if (id.empty()) {
            QMessageBox::warning(this, windowTitle(),
                                 tr("The secret was not stored: %1").arg(m_context->lastError()));
            return;
        }
        m_credentialId = id;
        m_secret->clear();
    }
    QDialog::accept();
}

// ---------------------------------------------------------------------------------------------
// SessionManagerDialog

SessionManagerDialog::SessionManagerDialog(AppContext* context, QWidget* parent)
    : QDialog(parent), m_context(context), m_tree(new QTreeWidget(this)) {
    setWindowTitle(tr("Sessions"));
    auto* layout = new QHBoxLayout(this);
    m_tree->setColumnCount(2);
    m_tree->setHeaderLabels({tr("Name"), tr("Target")});
    m_tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_tree->setRootIsDecorated(true);
    layout->addWidget(m_tree, 1);

    auto* buttons = new QVBoxLayout;
    auto addButton = [&](const QString& text, auto slot) {
        auto* button = new QPushButton(text, this);
        connect(button, &QPushButton::clicked, this, slot);
        buttons->addWidget(button);
        return button;
    };
    QPushButton* open = addButton(tr("Open"), [this] { openSelected(); });
    open->setDefault(true);
    addButton(tr("New SSH..."), [this] { createSession(SessionKind::Ssh); });
    addButton(tr("New Telnet..."), [this] { createSession(SessionKind::Telnet); });
    addButton(tr("New Serial..."), [this] { createSession(SessionKind::Serial); });
    addButton(tr("New Local..."), [this] { createSession(SessionKind::Local); });
    addButton(tr("Edit..."), [this] { editSelected(); });
    addButton(tr("Duplicate"), [this] { duplicateSelected(); });
    addButton(tr("Delete"), [this] { deleteSelected(); });
    buttons->addStretch(1);
    addButton(tr("Close"), [this] { reject(); });
    layout->addLayout(buttons);
    connect(m_tree, &QTreeWidget::itemDoubleClicked, this, [this] { openSelected(); });
    reload();
    resize(640, 420);
}

void SessionManagerDialog::reload() {
    const QString selected = selectedId();
    m_tree->clear();
    if (!m_context) {
        return;
    }
    QHash<QString, QTreeWidgetItem*> folders;
    auto folderItem = [&](const QString& path) -> QTreeWidgetItem* {
        if (path.isEmpty()) {
            return m_tree->invisibleRootItem();
        }
        QTreeWidgetItem* parent = m_tree->invisibleRootItem();
        QString prefix;
        for (const QString& part : path.split(QLatin1Char('/'), Qt::SkipEmptyParts)) {
            prefix += QLatin1Char('/') + part;
            if (!folders.contains(prefix)) {
                auto* item = new QTreeWidgetItem(parent, {part});
                item->setFirstColumnSpanned(true);
                QFont font = item->font(0);
                font.setBold(true);
                item->setFont(0, font);
                item->setExpanded(true);
                folders.insert(prefix, item);
            }
            parent = folders.value(prefix);
        }
        return parent;
    };
    for (const auto& profile : m_context->sessions().profiles()) {
        auto* item = new QTreeWidgetItem(folderItem(QString::fromStdString(profile.group)),
                                         {QString::fromStdString(profile.name), QString::fromStdString(profile.target())});
        item->setData(0, Qt::UserRole, QString::fromStdString(profile.id));
        item->setToolTip(0, QString::fromStdString(profile.notes));
    }
    m_tree->expandAll();
    selectId(selected);
}

QString SessionManagerDialog::selectedId() const {
    const QTreeWidgetItem* item = m_tree->currentItem();
    return item ? item->data(0, Qt::UserRole).toString() : QString();
}

void SessionManagerDialog::selectId(const QString& id) {
    if (id.isEmpty()) {
        return;
    }
    for (QTreeWidgetItemIterator it(m_tree); *it; ++it) {
        if ((*it)->data(0, Qt::UserRole).toString() == id) {
            m_tree->setCurrentItem(*it);
            return;
        }
    }
}

bool SessionManagerDialog::editProfile(SessionProfile profile, const QString& title) {
    SessionEditDialog dialog(m_context, profile, this);
    dialog.setWindowTitle(title);
    if (dialog.exec() != QDialog::Accepted) {
        return false;
    }
    const std::string id = m_context->sessions().upsert(dialog.profile());
    m_context->saveSessions();
    reload();
    selectId(QString::fromStdString(id));
    return true;
}

QString SessionEditDialog::titleForNew(SessionKind kind) {
    switch (kind) {
    case SessionKind::Ssh: return tr("New SSH Session");
    case SessionKind::Telnet: return tr("New Telnet Session");
    case SessionKind::Serial: return tr("New Serial Session");
    case SessionKind::Local: break;
    }
    return tr("New Local Session");
}

void SessionManagerDialog::createSession(SessionKind kind) {
    SessionProfile profile;
    profile.kind = kind;
    editProfile(profile, SessionEditDialog::titleForNew(kind));
}

void SessionManagerDialog::editSelected() {
    if (const auto* profile = m_context->sessions().find(selectedId().toStdString())) {
        editProfile(*profile, tr("Edit Session"));
    }
}

void SessionManagerDialog::duplicateSelected() {
    if (const auto id = m_context->sessions().duplicate(selectedId().toStdString())) {
        m_context->saveSessions();
        reload();
        selectId(QString::fromStdString(*id));
    }
}

void SessionManagerDialog::deleteSelected() {
    const auto* profile = m_context->sessions().find(selectedId().toStdString());
    if (!profile) {
        return;
    }
    const auto answer = QMessageBox::question(this, windowTitle(),
                                              tr("Delete the session \"%1\"?").arg(QString::fromStdString(profile->name)));
    if (answer == QMessageBox::Yes) {
        m_context->sessions().remove(profile->id);
        m_context->saveSessions();
        reload();
    }
}

void SessionManagerDialog::openSelected() {
    if (const auto* profile = m_context->sessions().find(selectedId().toStdString())) {
        const SessionProfile copy = *profile;
        accept();
        Q_EMIT openRequested(copy);
    }
}
