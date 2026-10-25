#include "SftpPanel.h"

#include "GuiInteraction.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QDragEnterEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QPointer>
#include <QProgressBar>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

using wt::ssh::SftpEntry;

namespace {

QString permissionString(std::uint32_t mode, bool directory, bool symlink) {
    QString text = symlink ? QStringLiteral("l") : directory ? QStringLiteral("d") : QStringLiteral("-");
    const char* letters = "rwxrwxrwx";
    for (int bit = 8; bit >= 0; --bit) {
        text += (mode & (1u << bit)) ? QLatin1Char(letters[8 - bit]) : QLatin1Char('-');
    }
    return text;
}

QToolButton* toolButton(const QString& text, const QString& tip, QWidget* parent) {
    auto* button = new QToolButton(parent);
    button->setText(text);
    button->setToolTip(tip);
    button->setAutoRaise(true);
    return button;
}

} // namespace

// Owns the SFTP session; runs queued jobs on its thread. The thread keeps the worker alive until
// the current job returns, so shutting down never blocks the GUI (or joins the worker itself).
struct SftpPanel::Worker : std::enable_shared_from_this<SftpPanel::Worker> {
    std::thread thread;
    std::mutex mutex;
    std::condition_variable wake;
    std::deque<Job> jobs;
    bool stop = false;
    std::atomic<bool> cancelled{false};
    std::atomic<bool> transferCancelled{false};
    wt::ssh::Aborter aborter;
    std::unique_ptr<wt::ssh::SftpSession> sftp;
    std::shared_ptr<wt::ssh::SshConnection> connection;

    void start() {
        thread = std::thread([self = shared_from_this()] { self->run(); });
    }
    // Stops after the current job (aborting a pending connect); the thread then drops its
    // reference and the worker is destroyed by whoever releases it last.
    void shutdown() {
        {
            std::lock_guard lock(mutex);
            stop = true;
            jobs.clear();
        }
        cancelled = true;
        transferCancelled = true;
        aborter.abort();
        wake.notify_all();
        if (thread.joinable()) {
            thread.detach();
        }
    }
    void post(Job job) {
        {
            std::lock_guard lock(mutex);
            jobs.push_back(std::move(job));
        }
        wake.notify_one();
    }
    void run() {
        for (;;) {
            Job job;
            {
                std::unique_lock lock(mutex);
                wake.wait(lock, [this] { return stop || !jobs.empty(); });
                if (stop) {
                    sftp.reset(); // close the session on this thread
                    connection.reset();
                    return;
                }
                job = std::move(jobs.front());
                jobs.pop_front();
            }
            job();
        }
    }
};

SftpPanel::SftpPanel(QWidget* parent)
    : QWidget(parent),
      m_pathEdit(new QLineEdit(this)),
      m_list(new QTreeWidget(this)),
      m_progress(new QProgressBar(this)),
      m_status(new QLabel(this)),
      m_cancel(toolButton(QStringLiteral("✕"), tr("Cancel the transfer"), this)) {
    setAcceptDrops(true);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(2, 2, 2, 2);
    auto* bar = new QHBoxLayout;
    QToolButton* up = toolButton(QStringLiteral("↑"), tr("Parent directory"), this);
    QToolButton* refresh = toolButton(QStringLiteral("⟳"), tr("Refresh"), this);
    QToolButton* home = toolButton(QStringLiteral("⌂"), tr("Home directory"), this);
    QToolButton* mkdir = toolButton(tr("New"), tr("New folder"), this);
    QToolButton* upload = toolButton(tr("Upload"), tr("Upload files"), this);
    QToolButton* download = toolButton(tr("Download"), tr("Download the selection"), this);
    QToolButton* remove = toolButton(tr("Delete"), tr("Delete the selection"), this);
    for (QToolButton* button : {up, refresh, home, mkdir, upload, download, remove}) {
        bar->addWidget(button);
    }
    bar->addStretch(1);
    layout->addLayout(bar);
    m_pathEdit->setObjectName(QStringLiteral("remotePath"));
    layout->addWidget(m_pathEdit);
    m_list->setColumnCount(4);
    m_list->setHeaderLabels({tr("Name"), tr("Size"), tr("Modified"), tr("Permissions")});
    m_list->setRootIsDecorated(false);
    m_list->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_list->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_list->setContextMenuPolicy(Qt::CustomContextMenu);
    layout->addWidget(m_list, 1);
    auto* statusRow = new QHBoxLayout;
    m_progress->setVisible(false);
    m_cancel->setVisible(false);
    statusRow->addWidget(m_status, 1);
    statusRow->addWidget(m_progress, 1);
    statusRow->addWidget(m_cancel);
    layout->addLayout(statusRow);

    connect(up, &QToolButton::clicked, this, &SftpPanel::goUp);
    connect(refresh, &QToolButton::clicked, this, &SftpPanel::refresh);
    connect(home, &QToolButton::clicked, this, [this] { navigate(QStringLiteral(".")); });
    connect(mkdir, &QToolButton::clicked, this, &SftpPanel::promptMakeDirectory);
    connect(upload, &QToolButton::clicked, this, &SftpPanel::chooseUploads);
    connect(download, &QToolButton::clicked, this, &SftpPanel::chooseDownloadTarget);
    connect(remove, &QToolButton::clicked, this, &SftpPanel::confirmAndRemove);
    connect(m_cancel, &QToolButton::clicked, this, &SftpPanel::cancelTransfer);
    connect(m_pathEdit, &QLineEdit::returnPressed, this, [this] { navigate(m_pathEdit->text()); });
    connect(m_list, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* item) {
        if (item->data(0, Qt::UserRole).toBool()) {
            navigate(QString::fromStdString(wt::ssh::joinRemotePath(m_path.toStdString(), item->text(0).toStdString())));
        }
    });
    connect(m_list, &QTreeWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        QMenu menu(this);
        menu.addAction(tr("Download..."), this, &SftpPanel::chooseDownloadTarget);
        menu.addAction(tr("Rename..."), this, &SftpPanel::promptRename);
        menu.addAction(tr("Delete"), this, &SftpPanel::confirmAndRemove);
        menu.addSeparator();
        menu.addAction(tr("New Folder..."), this, &SftpPanel::promptMakeDirectory);
        menu.addAction(tr("Upload..."), this, &SftpPanel::chooseUploads);
        menu.addAction(tr("Refresh"), this, &SftpPanel::refresh);
        menu.exec(m_list->viewport()->mapToGlobal(pos));
    });
    m_status->setText(tr("Not connected"));
}

SftpPanel::~SftpPanel() {
    if (m_worker) {
        m_worker->shutdown();
    }
}

void SftpPanel::setBusy(bool busy, const QString& text) {
    if (!text.isEmpty()) {
        m_status->setText(text);
    }
    if (m_busy != busy) {
        m_busy = busy;
        Q_EMIT busyChanged(busy);
    }
    if (!busy) {
        m_progress->setVisible(false);
        m_cancel->setVisible(false);
    }
}

void SftpPanel::onProgress(quint64 done, quint64 total) {
    m_progress->setVisible(true);
    m_cancel->setVisible(true);
    m_progress->setRange(0, 1000);
    m_progress->setValue(total > 0 ? static_cast<int>(done * 1000 / total) : 0);
    m_progress->setFormat(QStringLiteral("%1 / %2").arg(QLocale().formattedDataSize(static_cast<qint64>(done)),
                                                         QLocale().formattedDataSize(static_cast<qint64>(total))));
}

void SftpPanel::connectTo(const SshPlan& plan, QWidget* dialogParent) {
    if (m_worker) {
        m_worker->shutdown(); // closes any previous connection
    }
    m_worker = std::make_shared<Worker>();
    m_worker->start();
    const quint64 generation = ++m_generation;
    m_connected = false;
    m_pending = 0;
    m_list->clear();
    m_target = plan.description;
    setBusy(true, tr("Connecting to %1...").arg(plan.description));
    QPointer<SftpPanel> self(this);
    Worker* worker = m_worker.get();
    std::shared_ptr<SshPrompter> prompter = SshPrompter::create(dialogParent);
    ++m_pending;
    worker->post([self, worker, generation, prompter, plan = SshPlan(plan)]() mutable {
        GuiInteraction ui(prompter, &worker->cancelled);
        std::string error;
        worker->connection = connectPlan(plan, ui, &error, &worker->aborter);
        worker->aborter.detach();
        std::string home;
        if (worker->connection) {
            worker->connection->setBlocking(true);
            worker->sftp = worker->connection->openSftp(&error);
            if (worker->sftp) {
                if (auto status = worker->sftp->realPath(".", home); !status) {
                    error = status.message();
                    worker->sftp.reset();
                }
            }
        }
        std::vector<SftpEntry> entries;
        if (worker->sftp) {
            if (auto status = worker->sftp->list(home, entries); !status) {
                error = status.message();
            }
        }
        const bool ok = worker->sftp != nullptr;
        const QString message = QString::fromStdString(error);
        const QString path = QString::fromStdString(home);
        QMetaObject::invokeMethod(QCoreApplication::instance(), [self, generation, ok, message, path, entries] {
            SftpPanel* panel = self.data();
            if (!panel || panel->m_generation != generation) {
                return;
            }
            --panel->m_pending;
            panel->m_connected = ok;
            panel->setBusy(panel->m_pending > 0, ok ? QString() : message);
            if (ok) {
                panel->showEntries(path, entries);
                Q_EMIT panel->connectedTo(path);
            }
            Q_EMIT panel->operationFinished(ok, message);
        });
    });
}

void SftpPanel::disconnectFromServer() {
    if (m_worker) {
        m_worker->shutdown();
    }
    m_worker.reset();
    ++m_generation;
    m_connected = false;
    m_pending = 0;
    m_list->clear();
    m_target.clear();
    setBusy(false, tr("Not connected"));
}

void SftpPanel::enqueue(const QString& description, std::function<wt::core::Status()> job,
                        std::function<void()> onSuccess) {
    if (!m_worker || !m_connected) {
        Q_EMIT operationFinished(false, tr("Not connected"));
        return;
    }
    ++m_pending;
    setBusy(true, description);
    QPointer<SftpPanel> self(this);
    Worker* worker = m_worker.get();
    const quint64 generation = m_generation;
    worker->post([self, worker, generation, job = std::move(job), onSuccess = std::move(onSuccess)] {
        const wt::core::Status status = worker->sftp ? job() : wt::core::Status::error("not connected");
        const bool ok = status.isOk();
        const QString message = QString::fromStdString(status.message());
        QMetaObject::invokeMethod(QCoreApplication::instance(), [self, generation, ok, message, onSuccess] {
            SftpPanel* panel = self.data();
            if (!panel || panel->m_generation != generation) {
                return;
            }
            --panel->m_pending;
            panel->setBusy(panel->m_pending > 0, ok ? tr("Done") : message);
            if (ok && onSuccess) {
                onSuccess();
            }
            Q_EMIT panel->operationFinished(ok, message);
        });
    });
}

void SftpPanel::navigate(const QString& requested) {
    if (!m_worker || !m_connected) {
        return;
    }
    Worker* worker = m_worker.get();
    auto entries = std::make_shared<std::vector<SftpEntry>>();
    auto resolved = std::make_shared<std::string>();
    const std::string path = requested.toStdString();
    enqueue(
        tr("Listing %1...").arg(requested),
        [worker, entries, resolved, path] {
            if (auto status = worker->sftp->realPath(path, *resolved); !status) {
                return status;
            }
            return worker->sftp->list(*resolved, *entries);
        },
        [this, entries, resolved] { showEntries(QString::fromStdString(*resolved), *entries); });
}

void SftpPanel::refresh() {
    navigate(m_path.isEmpty() ? QStringLiteral(".") : m_path);
}

void SftpPanel::goUp() {
    navigate(QString::fromStdString(wt::ssh::remoteParent(m_path.toStdString())));
}

void SftpPanel::showEntries(const QString& path, const std::vector<SftpEntry>& entries) {
    m_path = path;
    m_pathEdit->setText(path);
    m_list->clear();
    for (const SftpEntry& entry : entries) {
        auto* item = new QTreeWidgetItem(m_list);
        item->setText(0, QString::fromStdString(entry.name));
        item->setData(0, Qt::UserRole, entry.isDirectory);
        item->setIcon(0, style()->standardIcon(entry.isDirectory ? QStyle::SP_DirIcon : QStyle::SP_FileIcon));
        item->setText(1, entry.isDirectory ? QString() : QLocale().formattedDataSize(static_cast<qint64>(entry.size)));
        item->setData(1, Qt::UserRole, static_cast<qulonglong>(entry.size));
        item->setText(2, QDateTime::fromSecsSinceEpoch(entry.modified).toString(QStringLiteral("yyyy-MM-dd HH:mm")));
        item->setText(3, permissionString(entry.permissions, entry.isDirectory, entry.isSymlink));
    }
    m_status->setText(tr("%n item(s)", nullptr, static_cast<int>(entries.size())));
    Q_EMIT listed(path);
}

QStringList SftpPanel::entryNames() const {
    QStringList names;
    for (int i = 0; i < m_list->topLevelItemCount(); ++i) {
        names << m_list->topLevelItem(i)->text(0);
    }
    return names;
}

QStringList SftpPanel::selectedNames() const {
    QStringList names;
    for (QTreeWidgetItem* item : m_list->selectedItems()) {
        names << item->text(0);
    }
    return names;
}

void SftpPanel::cancelTransfer() {
    if (m_worker) {
        m_worker->transferCancelled = true;
    }
}

void SftpPanel::uploadFiles(const QStringList& localFiles) {
    if (!m_worker || !m_connected) {
        Q_EMIT operationFinished(false, tr("Not connected"));
        return;
    }
    for (const QString& local : localFiles) {
        const QFileInfo info(local);
        if (!info.isFile()) {
            Q_EMIT operationFinished(false, tr("%1 is not a file").arg(local));
            continue;
        }
        Worker* worker = m_worker.get();
        QPointer<SftpPanel> self(this);
        const std::string remote = wt::ssh::joinRemotePath(m_path.toStdString(), info.fileName().toStdString());
        const std::string localPath = local.toStdString();
        worker->transferCancelled = false;
        enqueue(
            tr("Uploading %1...").arg(info.fileName()),
            [worker, self, remote, localPath] {
                return worker->sftp->upload(localPath, remote, [self, worker](std::uint64_t done, std::uint64_t total) {
                    QMetaObject::invokeMethod(QCoreApplication::instance(), [self, done, total] {
                        if (SftpPanel* panel = self.data()) {
                            panel->onProgress(done, total);
                        }
                    });
                    return !worker->transferCancelled.load();
                });
            },
            [this] { refresh(); });
    }
}

void SftpPanel::download(const QString& remoteName, const QString& localPath) {
    if (!m_worker || !m_connected) {
        Q_EMIT operationFinished(false, tr("Not connected"));
        return;
    }
    Worker* worker = m_worker.get();
    QPointer<SftpPanel> self(this);
    const std::string remote = wt::ssh::joinRemotePath(m_path.toStdString(), remoteName.toStdString());
    const std::string local = localPath.toStdString();
    worker->transferCancelled = false;
    enqueue(tr("Downloading %1...").arg(remoteName), [worker, self, remote, local] {
        return worker->sftp->download(remote, local, [self, worker](std::uint64_t done, std::uint64_t total) {
            QMetaObject::invokeMethod(QCoreApplication::instance(), [self, done, total] {
                if (SftpPanel* panel = self.data()) {
                    panel->onProgress(done, total);
                }
            });
            return !worker->transferCancelled.load();
        });
    });
}

void SftpPanel::downloadSelected(const QString& localDirectory) {
    for (QTreeWidgetItem* item : m_list->selectedItems()) {
        if (!item->data(0, Qt::UserRole).toBool()) {
            download(item->text(0), QDir(localDirectory).filePath(item->text(0)));
        }
    }
}

void SftpPanel::makeDirectory(const QString& name) {
    Worker* worker = m_worker.get();
    const std::string path = wt::ssh::joinRemotePath(m_path.toStdString(), name.toStdString());
    enqueue(tr("Creating %1...").arg(name), [worker, path] { return worker->sftp->makeDirectory(path); },
            [this] { refresh(); });
}

void SftpPanel::removeEntries(const QStringList& names) {
    Worker* worker = m_worker.get();
    std::vector<std::string> paths;
    for (const QString& name : names) {
        paths.push_back(wt::ssh::joinRemotePath(m_path.toStdString(), name.toStdString()));
    }
    enqueue(
        tr("Deleting..."),
        [worker, paths] {
            // Depth-first removal of directories.
            std::function<wt::core::Status(const std::string&)> removePath = [&](const std::string& path) {
                SftpEntry info;
                if (auto status = worker->sftp->stat(path, info); !status) {
                    return status;
                }
                if (!info.isDirectory || info.isSymlink) {
                    return worker->sftp->removeFile(path);
                }
                std::vector<SftpEntry> children;
                if (auto status = worker->sftp->list(path, children); !status) {
                    return status;
                }
                for (const SftpEntry& child : children) {
                    const std::string childPath = wt::ssh::joinRemotePath(path, child.name);
                    auto status = child.isDirectory && !child.isSymlink ? removePath(childPath)
                                                                        : worker->sftp->removeFile(childPath);
                    if (!status) {
                        return status;
                    }
                }
                return worker->sftp->removeDirectory(path);
            };
            for (const std::string& path : paths) {
                if (auto status = removePath(path); !status) {
                    return status;
                }
            }
            return wt::core::Status::ok();
        },
        [this] { refresh(); });
}

void SftpPanel::renameEntry(const QString& from, const QString& to) {
    Worker* worker = m_worker.get();
    const std::string source = wt::ssh::joinRemotePath(m_path.toStdString(), from.toStdString());
    const std::string target = to.startsWith(QLatin1Char('/'))
                                   ? to.toStdString()
                                   : wt::ssh::joinRemotePath(m_path.toStdString(), to.toStdString());
    enqueue(tr("Renaming %1...").arg(from), [worker, source, target] { return worker->sftp->rename(source, target); },
            [this] { refresh(); });
}

void SftpPanel::confirmAndRemove() {
    const QStringList names = selectedNames();
    if (names.isEmpty()) {
        return;
    }
    const auto answer = QMessageBox::question(this, tr("Delete"),
                                              tr("Delete %n item(s) on the server? This cannot be undone.", nullptr,
                                                 static_cast<int>(names.size())));
    if (answer == QMessageBox::Yes) {
        removeEntries(names);
    }
}

void SftpPanel::promptRename() {
    const QStringList names = selectedNames();
    if (names.size() != 1) {
        return;
    }
    bool ok = false;
    const QString to = QInputDialog::getText(this, tr("Rename"), tr("New name:"), QLineEdit::Normal, names.first(), &ok);
    if (ok && !to.isEmpty() && to != names.first()) {
        renameEntry(names.first(), to);
    }
}

void SftpPanel::promptMakeDirectory() {
    bool ok = false;
    const QString name = QInputDialog::getText(this, tr("New Folder"), tr("Name:"), QLineEdit::Normal, {}, &ok);
    if (ok && !name.isEmpty()) {
        makeDirectory(name);
    }
}

void SftpPanel::chooseUploads() {
    const QStringList files = QFileDialog::getOpenFileNames(this, tr("Upload Files"), QDir::homePath());
    if (!files.isEmpty()) {
        uploadFiles(files);
    }
}

void SftpPanel::chooseDownloadTarget() {
    if (selectedNames().isEmpty()) {
        return;
    }
    const QString directory = QFileDialog::getExistingDirectory(this, tr("Download To"), QDir::homePath());
    if (!directory.isEmpty()) {
        downloadSelected(directory);
    }
}

void SftpPanel::dragEnterEvent(QDragEnterEvent* event) {
    if (m_connected && event->mimeData()->hasUrls()) {
        event->acceptProposedAction();
    }
}

void SftpPanel::dropEvent(QDropEvent* event) {
    QStringList files;
    for (const QUrl& url : event->mimeData()->urls()) {
        if (url.isLocalFile()) {
            files << url.toLocalFile();
        }
    }
    uploadFiles(files);
    event->acceptProposedAction();
}
