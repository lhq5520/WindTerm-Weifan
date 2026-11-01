#pragma once

#include "wt/session/SessionProfile.h"

#include <QDialog>

class QCheckBox;
class QComboBox;
class QLineEdit;
class QSpinBox;

// Edits one port forward (local -L, remote -R or dynamic SOCKS -D).
class ForwardEditDialog : public QDialog {
    Q_OBJECT

public:
    explicit ForwardEditDialog(const wt::session::PortForwardSettings& forward, QWidget* parent = nullptr);

    wt::session::PortForwardSettings forward() const;
    // Empty when the forward can be used.
    QString problem() const;
    void accept() override;

    // "Local", "Remote", "Dynamic (SOCKS)"
    static QString kindName(wt::session::ForwardKind kind);

private:
    void updateVisibility();

    wt::session::PortForwardSettings m_original;
    QComboBox* m_kind;
    QLineEdit* m_bindAddress;
    QSpinBox* m_bindPort;
    QLineEdit* m_targetHost;
    QSpinBox* m_targetPort;
    QLineEdit* m_description;
    QCheckBox* m_enabled;
};
