#include "TerminalTab.h"

#include "TerminalPane.h"

#include <QSplitter>
#include <QTimer>
#include <QVBoxLayout>

TerminalTab::TerminalTab(TerminalPane* first, QWidget* parent) : QWidget(parent), m_root(first) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(first);
    adopt(first);
    m_active = first;
    setFocusProxy(first);
}

void TerminalTab::adopt(TerminalPane* pane) {
    connect(pane, &TerminalPane::activated, this, [this](TerminalPane* p) {
        if (m_active != p) {
            m_active = p;
            setFocusProxy(p);
            Q_EMIT activePaneChanged(p);
            Q_EMIT titleChanged();
        }
    });
    connect(pane, &TerminalPane::titleChanged, this, [this, pane] {
        if (pane == m_active) {
            Q_EMIT titleChanged();
        }
    });
}

void TerminalTab::collectPanes(QWidget* widget, QList<TerminalPane*>& out) const {
    if (auto* pane = qobject_cast<TerminalPane*>(widget)) {
        out.append(pane);
    } else if (auto* splitter = qobject_cast<QSplitter*>(widget)) {
        for (int i = 0; i < splitter->count(); ++i) {
            collectPanes(splitter->widget(i), out);
        }
    }
}

QList<TerminalPane*> TerminalTab::panes() const {
    QList<TerminalPane*> out;
    collectPanes(m_root, out);
    return out;
}

TerminalPane* TerminalTab::activePane() const {
    if (m_active) {
        return m_active;
    }
    const auto all = panes();
    return all.isEmpty() ? nullptr : all.first();
}

void TerminalTab::setActivePane(TerminalPane* pane) {
    if (!pane) {
        return;
    }
    m_active = pane;
    setFocusProxy(pane);
    pane->setFocus(Qt::OtherFocusReason);
    Q_EMIT activePaneChanged(pane);
    Q_EMIT titleChanged();
}

void TerminalTab::split(TerminalPane* target, Qt::Orientation orientation, TerminalPane* newPane) {
    adopt(newPane);
    auto* parentSplitter = qobject_cast<QSplitter*>(target->parentWidget());
    if (parentSplitter && parentSplitter->orientation() == orientation) {
        // Same direction: add a sibling and share the space evenly.
        const int index = parentSplitter->indexOf(target);
        parentSplitter->insertWidget(index + 1, newPane);
        QList<int> sizes;
        const int total = orientation == Qt::Horizontal ? parentSplitter->width() : parentSplitter->height();
        for (int i = 0; i < parentSplitter->count(); ++i) {
            sizes << std::max(1, total / parentSplitter->count());
        }
        parentSplitter->setSizes(sizes);
    } else {
        auto* splitter = new QSplitter(orientation);
        splitter->setChildrenCollapsible(false);
        splitter->setHandleWidth(3);
        if (parentSplitter) {
            const int index = parentSplitter->indexOf(target);
            const QList<int> sizes = parentSplitter->sizes();
            parentSplitter->replaceWidget(index, splitter);
            parentSplitter->setSizes(sizes);
        } else {
            delete layout()->replaceWidget(target, splitter); // the replaced item is ours to delete
            m_root = splitter;
        }
        splitter->addWidget(target);
        splitter->addWidget(newPane);
        const int total = orientation == Qt::Horizontal ? target->width() : target->height();
        splitter->setSizes({std::max(1, total / 2), std::max(1, total / 2)});
        target->show();
    }
    newPane->show();
    setActivePane(newPane);
}

void TerminalTab::closePane(TerminalPane* pane) {
    const QList<TerminalPane*> all = panes();
    if (!all.contains(pane)) {
        return;
    }
    const int index = static_cast<int>(all.indexOf(pane));
    auto* splitter = qobject_cast<QSplitter*>(pane->parentWidget());
    pane->hide();
    pane->setParent(nullptr);
    pane->deleteLater();
    if (!splitter) {
        m_root = nullptr;
        Q_EMIT empty();
        return;
    }
    // Collapse splitters that are left with a single child.
    while (splitter && splitter->count() == 1) {
        QWidget* only = splitter->widget(0);
        auto* grandParent = qobject_cast<QSplitter*>(splitter->parentWidget());
        if (grandParent) {
            const QList<int> sizes = grandParent->sizes();
            grandParent->replaceWidget(grandParent->indexOf(splitter), only);
            grandParent->setSizes(sizes);
        } else {
            delete layout()->replaceWidget(splitter, only);
            m_root = only;
        }
        only->show();
        splitter->deleteLater();
        splitter = grandParent;
    }
    const QList<TerminalPane*> remaining = panes();
    if (remaining.isEmpty()) {
        Q_EMIT empty();
        return;
    }
    if (m_active == pane || !m_active) {
        setActivePane(remaining[std::clamp(index - 1, 0, static_cast<int>(remaining.size()) - 1)]);
    }
}

void TerminalTab::focusNextPane(bool forward) {
    const QList<TerminalPane*> all = panes();
    if (all.size() < 2) {
        return;
    }
    const int index = static_cast<int>(all.indexOf(activePane()));
    const int count = static_cast<int>(all.size());
    setActivePane(all[(index + (forward ? 1 : count - 1)) % count]);
}

QString TerminalTab::title() const {
    TerminalPane* pane = activePane();
    return pane ? pane->title() : QString();
}

namespace {

wt::session::LayoutNode captureNode(QWidget* widget) {
    wt::session::LayoutNode node;
    if (auto* pane = qobject_cast<TerminalPane*>(widget)) {
        node.pane = pane->paneState();
    } else if (auto* splitter = qobject_cast<QSplitter*>(widget)) {
        node.split = true;
        node.vertical = splitter->orientation() == Qt::Vertical;
        for (int i = 0; i < splitter->count(); ++i) {
            node.children.push_back(captureNode(splitter->widget(i)));
        }
        const QList<int> sizes = splitter->sizes();
        node.sizes.assign(sizes.begin(), sizes.end());
    }
    return node;
}

} // namespace

wt::session::LayoutNode TerminalTab::captureLayout() const {
    return captureNode(m_root);
}
