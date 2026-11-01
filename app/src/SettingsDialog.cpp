#include "SettingsDialog.h"

#include "TerminalWidget.h"
#include "TriggerEditDialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFontComboBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QTabWidget>
#include <QTableWidget>
#include <QTreeWidget>
#include <QVBoxLayout>

using namespace wt::session;

namespace {

constexpr char kSequenceSeparator[] = "; ";

QString joinSequences(const std::vector<std::string>& sequences) {
    QStringList list;
    for (const auto& sequence : sequences) {
        list << QString::fromStdString(sequence);
    }
    return list.join(QString::fromLatin1(kSequenceSeparator));
}

} // namespace

SettingsDialog::SettingsDialog(const Settings& settings, QWidget* parent) : QDialog(parent), m_original(settings) {
    setWindowTitle(tr("Settings"));
    auto* layout = new QVBoxLayout(this);
    auto* pages = new QTabWidget(this);
    pages->addTab(createAppearancePage(), tr("Appearance"));
    pages->addTab(createTerminalPage(), tr("Terminal"));
    pages->addTab(createKeyboardPage(), tr("Keyboard"));
    pages->addTab(createSshPage(), tr("SSH && Security"));
    pages->addTab(createLoggingPage(), tr("Logging && Triggers"));
    layout->addWidget(pages);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &SettingsDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &SettingsDialog::reject);
    layout->addWidget(buttons);
    resize(640, 520);
}

QWidget* SettingsDialog::createAppearancePage() {
    auto* page = new QWidget(this);
    auto* form = new QFormLayout(page);
    const TerminalSettings& t = m_original.terminal;

    m_systemFont = new QCheckBox(tr("Use the system fixed-width font"), page);
    m_systemFont->setObjectName(QStringLiteral("systemFont"));
    m_systemFont->setChecked(t.fontFamily.empty());
    m_font = new QFontComboBox(page);
    m_font->setObjectName(QStringLiteral("fontFamily"));
    m_font->setFontFilters(QFontComboBox::MonospacedFonts);
    m_font->setCurrentFont(t.fontFamily.empty() ? TerminalAppearance::defaultFont()
                                                : QFont(QString::fromStdString(t.fontFamily)));
    m_font->setEnabled(!t.fontFamily.empty());
    connect(m_systemFont, &QCheckBox::toggled, m_font, [this](bool on) { m_font->setEnabled(!on); });
    m_fontSize = new QDoubleSpinBox(page);
    m_fontSize->setObjectName(QStringLiteral("fontSize"));
    m_fontSize->setRange(0, 96);
    m_fontSize->setDecimals(1);
    m_fontSize->setSpecialValueText(tr("Default"));
    m_fontSize->setValue(t.fontSize);
    m_theme = new QComboBox(page);
    m_theme->setObjectName(QStringLiteral("theme"));
    for (const auto& theme : builtinThemes()) {
        m_theme->addItem(QString::fromStdString(theme.name));
    }
    for (const auto& theme : m_original.customThemes) {
        m_theme->addItem(QString::fromStdString(theme.name));
    }
    m_theme->setCurrentText(QString::fromStdString(t.theme));
    m_cursorStyle = new QComboBox(page);
    m_cursorStyle->setObjectName(QStringLiteral("cursorStyle"));
    m_cursorStyle->addItem(tr("Block"), static_cast<int>(CursorStyle::Block));
    m_cursorStyle->addItem(tr("Underline"), static_cast<int>(CursorStyle::Underline));
    m_cursorStyle->addItem(tr("Bar"), static_cast<int>(CursorStyle::Bar));
    m_cursorStyle->setCurrentIndex(m_cursorStyle->findData(static_cast<int>(t.cursorStyle)));
    m_cursorBlink = new QCheckBox(tr("Blinking cursor"), page);
    m_cursorBlink->setChecked(t.cursorBlink);

    form->addRow(m_systemFont);
    form->addRow(tr("Font:"), m_font);
    form->addRow(tr("Size (pt):"), m_fontSize);
    form->addRow(tr("Colour theme:"), m_theme);
    form->addRow(tr("Cursor:"), m_cursorStyle);
    form->addRow(QString(), m_cursorBlink);
    return page;
}

QWidget* SettingsDialog::createTerminalPage() {
    auto* page = new QWidget(this);
    auto* form = new QFormLayout(page);
    const TerminalSettings& t = m_original.terminal;
    m_scrollback = new QSpinBox(page);
    m_scrollback->setObjectName(QStringLiteral("scrollback"));
    m_scrollback->setRange(Settings::kMinScrollback, Settings::kMaxScrollback);
    m_scrollback->setSingleStep(1000);
    m_scrollback->setValue(t.scrollbackLines);
    m_bell = new QComboBox(page);
    m_bell->addItem(tr("Flash the window"), static_cast<int>(BellStyle::Visual));
    m_bell->addItem(tr("System sound"), static_cast<int>(BellStyle::Audible));
    m_bell->addItem(tr("Off"), static_cast<int>(BellStyle::None));
    m_bell->setCurrentIndex(m_bell->findData(static_cast<int>(t.bell)));
    m_copyOnSelect = new QCheckBox(tr("Copy selected text to the clipboard automatically"), page);
    m_copyOnSelect->setChecked(t.copyOnSelect);
    m_clipboardWrite = new QCheckBox(tr("Allow programs to set the clipboard (OSC 52)"), page);
    m_clipboardWrite->setObjectName(QStringLiteral("clipboardWrite"));
    m_clipboardWrite->setToolTip(tr("Remote programs could replace your clipboard contents. Reading the clipboard "
                                    "is never allowed."));
    m_clipboardWrite->setChecked(t.allowClipboardWrite);
    m_optionAsMeta = new QCheckBox(tr("Use Option as Meta key (macOS)"), page);
    m_optionAsMeta->setChecked(t.optionAsMeta);
    m_confirmClose = new QCheckBox(tr("Confirm before closing running sessions"), page);
    m_confirmClose->setChecked(m_original.confirmCloseRunning);
    form->addRow(tr("Scrollback lines:"), m_scrollback);
    form->addRow(tr("Bell:"), m_bell);
    form->addRow(m_copyOnSelect);
    form->addRow(m_clipboardWrite);
    form->addRow(m_optionAsMeta);
    form->addRow(m_confirmClose);
    return page;
}

QWidget* SettingsDialog::createKeyboardPage() {
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);
    auto* hint = new QLabel(tr("Separate alternative shortcuts with \";\". Leave empty to unbind."), page);
    hint->setWordWrap(true);
    layout->addWidget(hint);
    m_keys = new QTableWidget(static_cast<int>(allActions().size()), 2, page);
    m_keys->setObjectName(QStringLiteral("keybindings"));
    m_keys->setHorizontalHeaderLabels({tr("Command"), tr("Shortcuts")});
    m_keys->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_keys->horizontalHeader()->setStretchLastSection(true);
    m_keys->verticalHeader()->hide();
    layout->addWidget(m_keys, 1);
    m_keyStatus = new QLabel(page);
    m_keyStatus->setWordWrap(true);
    layout->addWidget(m_keyStatus);
    auto* reset = new QPushButton(tr("Restore Defaults"), page);
    connect(reset, &QPushButton::clicked, this, [this] { loadKeymap(Keymap::defaults()); });
    layout->addWidget(reset, 0, Qt::AlignLeft);
    loadKeymap(m_original.keymap);
    connect(m_keys, &QTableWidget::itemChanged, this, &SettingsDialog::updateKeymapStatus);
    return page;
}

QWidget* SettingsDialog::createSshPage() {
    auto* page = new QWidget(this);
    auto* form = new QFormLayout(page);
    auto* row = new QHBoxLayout;
    m_knownHosts = new QLineEdit(QString::fromStdString(m_original.knownHostsFile), page);
    m_knownHosts->setPlaceholderText(QStringLiteral("~/.ssh/known_hosts"));
    auto* browse = new QPushButton(tr("Browse..."), page);
    connect(browse, &QPushButton::clicked, this, [this] {
        const QString file = QFileDialog::getSaveFileName(this, tr("known_hosts file"), m_knownHosts->text(), {},
                                                          nullptr, QFileDialog::DontConfirmOverwrite);
        if (!file.isEmpty()) {
            m_knownHosts->setText(file);
        }
    });
    row->addWidget(m_knownHosts, 1);
    row->addWidget(browse);
    form->addRow(tr("Known hosts file:"), row);

    m_credentialStore = new QComboBox(page);
    m_credentialStore->setObjectName(QStringLiteral("credentialStore"));
    m_credentialStore->addItem(tr("Encrypted vault (master password)"), static_cast<int>(CredentialStoreKind::Vault));
    m_credentialStore->addItem(tr("System keychain"), static_cast<int>(CredentialStoreKind::System));
    m_credentialStore->setCurrentIndex(
        std::max(0, m_credentialStore->findData(static_cast<int>(m_original.security.credentialStore))));
    m_credentialStore->setToolTip(tr("macOS Keychain, Windows Credential Manager or the desktop keyring (Secret "
                                     "Service). Existing secrets stay where they are."));
    m_vaultLock = new QSpinBox(page);
    m_vaultLock->setObjectName(QStringLiteral("vaultLockMinutes"));
    m_vaultLock->setRange(0, 7 * 24 * 60);
    m_vaultLock->setValue(m_original.security.vaultLockMinutes);
    m_vaultLock->setSuffix(tr(" min"));
    m_vaultLock->setSpecialValueText(tr("Never"));
    m_restoreWorkspace = new QCheckBox(tr("Reopen the last windows, tabs and sessions on start"), page);
    m_restoreWorkspace->setObjectName(QStringLiteral("restoreWorkspace"));
    m_restoreWorkspace->setChecked(m_original.restoreWorkspace);
    form->addRow(tr("Remember passwords in:"), m_credentialStore);
    form->addRow(tr("Lock the vault when idle for:"), m_vaultLock);
    form->addRow(QString(), m_restoreWorkspace);
    return page;
}

QWidget* SettingsDialog::createLoggingPage() {
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);
    auto* form = new QFormLayout;
    const LoggingSettings& logging = m_original.logging;
    m_logAutoStart = new QCheckBox(tr("Log every session from its start"), page);
    m_logAutoStart->setObjectName(QStringLiteral("logAutoStart"));
    m_logAutoStart->setChecked(logging.autoStart);
    m_logDirectory = new QLineEdit(QString::fromStdString(logging.directory), page);
    m_logDirectory->setObjectName(QStringLiteral("logDirectory"));
    m_logDirectory->setPlaceholderText(tr("Documents/WindTerm-Weifan/Logs"));
    auto* browse = new QPushButton(tr("Browse..."), page);
    connect(browse, &QPushButton::clicked, this, [this] {
        const QString directory = QFileDialog::getExistingDirectory(this, tr("Log folder"), m_logDirectory->text());
        if (!directory.isEmpty()) {
            m_logDirectory->setText(directory);
        }
    });
    auto* directoryRow = new QHBoxLayout;
    directoryRow->addWidget(m_logDirectory, 1);
    directoryRow->addWidget(browse);
    m_logFileName = new QLineEdit(QString::fromStdString(logging.fileName), page);
    m_logFileName->setObjectName(QStringLiteral("logFileName"));
    m_logFileName->setToolTip(tr("%Y %m %d %H %M %S: date and time, %N: session name, %h: host"));
    m_logFormat = new QComboBox(page);
    m_logFormat->setObjectName(QStringLiteral("logFormat"));
    m_logFormat->addItem(tr("Plain text"), static_cast<int>(LogFormat::Text));
    m_logFormat->addItem(tr("Raw (with escape sequences)"), static_cast<int>(LogFormat::Raw));
    m_logFormat->setCurrentIndex(std::max(0, m_logFormat->findData(static_cast<int>(logging.format))));
    m_logTimestamps = new QCheckBox(tr("Start every line with the time"), page);
    m_logTimestamps->setChecked(logging.timestamps);
    form->addRow(QString(), m_logAutoStart);
    form->addRow(tr("Folder:"), directoryRow);
    form->addRow(tr("File name:"), m_logFileName);
    form->addRow(tr("Format:"), m_logFormat);
    form->addRow(QString(), m_logTimestamps);
    m_completion = new QCheckBox(tr("Suggest commands from the history while typing"), page);
    m_completion->setObjectName(QStringLiteral("completionEnabled"));
    m_completion->setChecked(m_original.completion.enabled);
    m_completionPrefix = new QSpinBox(page);
    m_completionPrefix->setRange(1, 20);
    m_completionPrefix->setValue(m_original.completion.minimumPrefix);
    m_completionPrefix->setSuffix(tr(" characters"));
    form->addRow(tr("Completion:"), m_completion);
    form->addRow(tr("Suggest after:"), m_completionPrefix);
    layout->addLayout(form);

    layout->addWidget(new QLabel(tr("Triggers (applied to every line of output):"), page));
    m_triggers = m_original.triggers;
    m_triggerList = new QTreeWidget(page);
    m_triggerList->setObjectName(QStringLiteral("triggerList"));
    m_triggerList->setRootIsDecorated(false);
    m_triggerList->setHeaderLabels({tr("Name"), tr("Pattern"), tr("Action")});
    connect(m_triggerList, &QTreeWidget::itemDoubleClicked, this,
            [this](QTreeWidgetItem* item) { editTrigger(m_triggerList->indexOfTopLevelItem(item)); });
    connect(m_triggerList, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem* item, int column) {
        const int index = m_triggerList->indexOfTopLevelItem(item);
        if (column == 0 && index >= 0 && index < static_cast<int>(m_triggers.size())) {
            m_triggers[static_cast<std::size_t>(index)].enabled = item->checkState(0) == Qt::Checked;
        }
    });
    layout->addWidget(m_triggerList, 1);
    auto* buttons = new QHBoxLayout;
    auto* add = new QPushButton(tr("Add..."), page);
    auto* edit = new QPushButton(tr("Edit..."), page);
    auto* remove = new QPushButton(tr("Remove"), page);
    connect(add, &QPushButton::clicked, this, [this] { editTrigger(-1); });
    connect(edit, &QPushButton::clicked, this,
            [this] { editTrigger(m_triggerList->indexOfTopLevelItem(m_triggerList->currentItem())); });
    connect(remove, &QPushButton::clicked, this, [this] {
        const int index = m_triggerList->indexOfTopLevelItem(m_triggerList->currentItem());
        if (index >= 0 && index < static_cast<int>(m_triggers.size())) {
            m_triggers.erase(m_triggers.begin() + index);
            reloadTriggers();
        }
    });
    buttons->addWidget(add);
    buttons->addWidget(edit);
    buttons->addWidget(remove);
    buttons->addStretch(1);
    layout->addLayout(buttons);
    reloadTriggers();
    return page;
}

void SettingsDialog::reloadTriggers() {
    const QSignalBlocker blocker(m_triggerList);
    m_triggerList->clear();
    for (const TriggerRule& rule : m_triggers) {
        auto* item = new QTreeWidgetItem(m_triggerList, {QString::fromStdString(rule.name), QString::fromStdString(rule.pattern),
                                                         TriggerEditDialog::actionName(rule.action)});
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(0, rule.enabled ? Qt::Checked : Qt::Unchecked);
    }
}

void SettingsDialog::editTrigger(int index) {
    const bool adding = index < 0 || index >= static_cast<int>(m_triggers.size());
    TriggerEditDialog dialog(adding ? TriggerRule{} : m_triggers[static_cast<std::size_t>(index)], this);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    if (adding) {
        m_triggers.push_back(dialog.rule());
    } else {
        m_triggers[static_cast<std::size_t>(index)] = dialog.rule();
    }
    reloadTriggers();
}

void SettingsDialog::setTriggers(std::vector<TriggerRule> triggers) {
    m_triggers = std::move(triggers);
    reloadTriggers();
}

void SettingsDialog::loadKeymap(const Keymap& keymap) {
    const QSignalBlocker blocker(m_keys);
    int row = 0;
    for (const ActionInfo& info : allActions()) {
        auto* name = new QTableWidgetItem(tr(info.title.data()));
        name->setFlags(name->flags() & ~Qt::ItemIsEditable);
        name->setData(Qt::UserRole, static_cast<int>(info.action));
        m_keys->setItem(row, 0, name);
        m_keys->setItem(row, 1, new QTableWidgetItem(joinSequences(keymap.sequences(info.action))));
        ++row;
    }
    updateKeymapStatus();
}

Keymap SettingsDialog::editedKeymap(QStringList* problems) const {
    Keymap keymap = m_original.keymap;
    for (int row = 0; row < m_keys->rowCount(); ++row) {
        const auto action = static_cast<Action>(m_keys->item(row, 0)->data(Qt::UserRole).toInt());
        std::vector<std::string> sequences;
        for (const QString& part : m_keys->item(row, 1)->text().split(QLatin1Char(';'), Qt::SkipEmptyParts)) {
            const QString trimmed = part.trimmed();
            const QKeySequence key = QKeySequence::fromString(trimmed, QKeySequence::PortableText);
            if (key.isEmpty() || key[0].key() == Qt::Key_unknown) {
                if (problems) {
                    *problems << tr("\"%1\" is not a valid shortcut").arg(trimmed);
                }
                continue;
            }
            sequences.push_back(key.toString(QKeySequence::PortableText).toStdString());
        }
        keymap.setSequences(action, std::move(sequences));
    }
    if (problems) {
        for (const auto& [sequence, actions] : keymap.conflicts()) {
            QStringList names;
            for (Action action : actions) {
                for (const auto& info : allActions()) {
                    if (info.action == action) {
                        names << tr(info.title.data());
                    }
                }
            }
            *problems << tr("%1 is used by: %2").arg(QString::fromStdString(sequence), names.join(QStringLiteral(", ")));
        }
    }
    return keymap;
}

QStringList SettingsDialog::keymapProblems() const {
    QStringList problems;
    editedKeymap(&problems);
    return problems;
}

void SettingsDialog::updateKeymapStatus() {
    const QStringList problems = keymapProblems();
    m_keyStatus->setText(problems.join(QLatin1Char('\n')));
    m_keyStatus->setStyleSheet(problems.isEmpty() ? QString() : QStringLiteral("color: #d9534f"));
}

Settings SettingsDialog::settings() const {
    Settings s = m_original;
    TerminalSettings& t = s.terminal;
    t.fontFamily = m_systemFont->isChecked() ? std::string() : m_font->currentFont().family().toStdString();
    t.fontSize = m_fontSize->value();
    t.theme = m_theme->currentText().toStdString();
    t.cursorStyle = static_cast<CursorStyle>(m_cursorStyle->currentData().toInt());
    t.cursorBlink = m_cursorBlink->isChecked();
    t.scrollbackLines = m_scrollback->value();
    t.bell = static_cast<BellStyle>(m_bell->currentData().toInt());
    t.copyOnSelect = m_copyOnSelect->isChecked();
    t.allowClipboardWrite = m_clipboardWrite->isChecked();
    t.optionAsMeta = m_optionAsMeta->isChecked();
    s.confirmCloseRunning = m_confirmClose->isChecked();
    s.keymap = editedKeymap(nullptr);
    s.knownHostsFile = m_knownHosts->text().trimmed().toStdString();
    s.logging.autoStart = m_logAutoStart->isChecked();
    s.logging.directory = m_logDirectory->text().trimmed().toStdString();
    s.logging.fileName = m_logFileName->text().trimmed().toStdString();
    if (s.logging.fileName.empty()) {
        s.logging.fileName = LoggingSettings{}.fileName;
    }
    s.logging.format = static_cast<LogFormat>(m_logFormat->currentData().toInt());
    s.logging.timestamps = m_logTimestamps->isChecked();
    s.completion.enabled = m_completion->isChecked();
    s.completion.minimumPrefix = m_completionPrefix->value();
    s.triggers = m_triggers;
    s.security.credentialStore = static_cast<CredentialStoreKind>(m_credentialStore->currentData().toInt());
    s.security.vaultLockMinutes = m_vaultLock->value();
    s.restoreWorkspace = m_restoreWorkspace->isChecked();
    return s;
}

void SettingsDialog::accept() {
    const QStringList problems = keymapProblems();
    if (!problems.isEmpty()) {
        const auto answer = QMessageBox::warning(this, windowTitle(),
                                                 tr("Some shortcuts have problems:\n%1\n\nSave anyway?")
                                                     .arg(problems.join(QLatin1Char('\n'))),
                                                 QMessageBox::Save | QMessageBox::Cancel);
        if (answer != QMessageBox::Save) {
            return;
        }
    }
    QDialog::accept();
}
