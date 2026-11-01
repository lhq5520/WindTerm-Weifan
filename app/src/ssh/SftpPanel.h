#pragma once

#include "SshBackend.h"

#include "wt/ssh/Sftp.h"

#include <QWidget>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

class QLabel;
class QLineEdit;
class QProgressBar;
class QToolButton;
class QTreeWidget;

// Remote file browser over SFTP. It opens its own SSH connection (with the secrets the terminal
// session already used) and runs all file operations on a worker thread, one at a time.
class SftpPanel : public QWidget {
    Q_OBJECT

public:
    explicit SftpPanel(QWidget* parent = nullptr);
    ~SftpPanel() override;

    // Connects to the target of `plan` and lists the home directory.
    void connectTo(const SshPlan& plan, QWidget* dialogParent);
    void disconnectFromServer();
    bool isConnected() const { return m_connected; }
    bool isBusy() const { return m_busy; }
    QString target() const { return m_target; }
    QString currentPath() const { return m_path; }
    QTreeWidget* list() const { return m_list; }
    QStringList entryNames() const;

    void navigate(const QString& path);
    void refresh();
    void goUp();
    void uploadFiles(const QStringList& localFiles); // into the current directory
    // Downloads a file, or a whole directory when `directory` is set.
    void download(const QString& remoteName, const QString& localPath, bool directory = false);
    void downloadSelected(const QString& localDirectory);
    void makeDirectory(const QString& name);
    // Removes files and (recursively) directories in the current directory, without asking.
    void removeEntries(const QStringList& names);
    void renameEntry(const QString& from, const QString& to);
    void cancelTransfer();

Q_SIGNALS:
    void connectedTo(const QString& home);
    void listed(const QString& path);
    // A queued operation completed; `ok` false with an error message on failure.
    void operationFinished(bool ok, const QString& message);
    void busyChanged(bool busy);

protected:
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;

private:
    struct Worker;
    using Job = std::function<void()>;
    void enqueue(const QString& description, std::function<wt::core::Status()> job,
                 std::function<void()> onSuccess = {});
    void showEntries(const QString& path, const std::vector<wt::ssh::SftpEntry>& entries);
    void setBusy(bool busy, const QString& text = {});
    void onProgress(quint64 done, quint64 total);
    QStringList selectedNames() const;
    void confirmAndRemove();
    void promptRename();
    void promptMakeDirectory();
    void chooseUploads();
    void chooseDownloadTarget();

    std::shared_ptr<Worker> m_worker; // also owned by its thread until that finishes
    quint64 m_generation = 0;        // identifies the current worker in queued results
    QLineEdit* m_pathEdit;
    QTreeWidget* m_list;
    QProgressBar* m_progress;
    QLabel* m_status;
    QToolButton* m_cancel;
    QString m_path;
    QString m_target;
    bool m_connected = false;
    bool m_busy = false;
    int m_pending = 0;
};
