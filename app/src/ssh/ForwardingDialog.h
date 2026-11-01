#pragma once

#include "wt/session/SessionProfile.h"

#include <QDialog>
#include <QPointer>
#include <QTimer>

class QLabel;
class QTreeWidget;
class SshBackend;

// Shows the port forwards of a connected SSH session with their state and connection counts, and
// adds or removes forwards while connected.
class ForwardingDialog : public QDialog {
    Q_OBJECT

public:
    explicit ForwardingDialog(SshBackend* backend, QWidget* parent = nullptr);

    QTreeWidget* list() const { return m_list; }
    // Adds a forward to the live connection (also kept for reconnects); returns its id.
    int addForward(const wt::session::PortForwardSettings& forward);
    void removeSelected();
    void refresh();

private:
    QPointer<SshBackend> m_backend;
    QTreeWidget* m_list;
    QLabel* m_summary;
    QTimer m_timer;
};
