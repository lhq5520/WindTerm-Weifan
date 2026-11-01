// Phase 3 GUI tests: tabs, split panes, search, themes, shortcuts, settings and saved sessions.
// Run with QT_QPA_PLATFORM=offscreen (set by CTest).

#include "AppContext.h"
#include "MainWindow.h"
#include "SearchBar.h"
#include "SessionDialogs.h"
#include "SettingsDialog.h"
#include "TerminalPane.h"
#include "TerminalSession.h"
#include "TerminalTab.h"
#include "TerminalWidget.h"

#include "wt/core/Json.h"
#include "wt/platform/Files.h"

#include <QApplication>
#include <QClipboard>
#include <QLineEdit>
#include <QMouseEvent>
#include <QSignalSpy>
#include <QSplitter>
#include <QStandardPaths>
#include <QTabWidget>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QTreeWidget>

using namespace wt::session;

namespace {

TerminalSession::Options shellOptions(const QString& script, int rows = 24, int columns = 80) {
    TerminalSession::Options options;
    options.program = QStringLiteral("/bin/sh");
    options.arguments = {QStringLiteral("-c"), script};
    options.rows = rows;
    options.columns = columns;
    return options;
}

bool screenContains(const TerminalSession& session, const char* text) {
    return session.terminal().screenText().find(text) != std::string::npos;
}

QPoint cellCenter(const TerminalWidget& widget, int row, int column) {
    const QSize cell = widget.cellSize();
    return {column * cell.width() + cell.width() / 2, row * cell.height() + cell.height() / 2};
}

void sendMouse(TerminalWidget& widget, QEvent::Type type, Qt::MouseButton button, Qt::MouseButtons buttons,
               const QPoint& pos, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
    QWidget* viewport = widget.viewport();
    QMouseEvent event(type, QPointF(pos), QPointF(viewport->mapToGlobal(pos)), button, buttons, modifiers);
    QApplication::sendEvent(viewport, &event);
}

// A context in a temporary directory with prompts disabled.
AppContext* makeContext(const QString& dir, QObject* parent) {
    auto* context = new AppContext(dir, parent);
    Settings settings = context->settings();
    settings.confirmCloseRunning = false;
    context->setSettings(settings);
    return context;
}

// Starts a window whose first tab runs `script`.
std::unique_ptr<MainWindow> startWindow(AppContext* context, const QString& script) {
    MainWindow::Options options;
    options.session = shellOptions(script);
    auto window = std::make_unique<MainWindow>(options, context);
    if (!window->start()) {
        return nullptr;
    }
    window->show();
    return window;
}

const QString kIdleShell = QStringLiteral("echo ready; exec sleep 30");

} // namespace

class WorkspaceTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void initTestCase() { QStandardPaths::setTestModeEnabled(true); }

    void tabsOpenAndCloseWithTheirShells() {
        QTemporaryDir dir;
        auto window = startWindow(makeContext(dir.path(), this), QStringLiteral("echo first; exec sleep 30"));
        QVERIFY(window);
        QCOMPARE(window->tabCount(), 1);
        QTRY_VERIFY_WITH_TIMEOUT(screenContains(*window->session(), "first"), 5000);

        TerminalPane* second = window->newTab();
        QCOMPARE(window->tabCount(), 2);
        QCOMPARE(window->activePane(), second);
        QTRY_VERIFY_WITH_TIMEOUT(second->session()->isRunning(), 5000);
        second->session()->sendBytes("exit 0\r");
        // The user's real login shell: its rc files can take seconds on a busy machine.
        QTRY_COMPARE_WITH_TIMEOUT(window->tabCount(), 1, 20000); // the tab closes with its shell
        QVERIFY(window->isVisible());

        window->action(Action::NewTab)->trigger();
        QCOMPARE(window->tabCount(), 2);
        window->action(Action::PreviousTab)->trigger();
        QCOMPARE(window->tabs()->currentIndex(), 0);
        window->action(Action::NextTab)->trigger();
        QCOMPARE(window->tabs()->currentIndex(), 1);
        window->action(Action::CloseTab)->trigger();
        QCOMPARE(window->tabCount(), 1);
    }

    void splitPanesFocusAndCollapse() {
        QTemporaryDir dir;
        auto window = startWindow(makeContext(dir.path(), this), kIdleShell);
        QVERIFY(window);
        TerminalTab* tab = window->currentTab();
        TerminalPane* first = tab->activePane();

        TerminalPane* right = window->splitActive(Qt::Horizontal);
        QVERIFY(right);
        QCOMPARE(tab->paneCount(), 2);
        QCOMPARE(tab->activePane(), right);
        auto* splitter = qobject_cast<QSplitter*>(right->parentWidget());
        QVERIFY(splitter);
        QCOMPARE(splitter->orientation(), Qt::Horizontal);

        TerminalPane* below = window->splitActive(Qt::Vertical);
        QCOMPARE(tab->paneCount(), 3);
        auto* inner = qobject_cast<QSplitter*>(below->parentWidget());
        QVERIFY(inner && inner->orientation() == Qt::Vertical);
        QCOMPARE(tab->panes(), QList<TerminalPane*>({first, right, below}));
        QTRY_VERIFY_WITH_TIMEOUT(below->session()->isRunning(), 5000);
        QVERIFY(window->tabs()->tabText(0).endsWith(QStringLiteral("[3]")));

        // Panes resize their sessions to their (smaller) share of the window.
        QTRY_VERIFY_WITH_TIMEOUT(first->session()->terminal().columns() < 80, 5000);

        tab->focusNextPane(true); // wraps to the first
        QCOMPARE(tab->activePane(), first);
        window->action(Action::FocusPreviousPane)->trigger();
        QCOMPARE(tab->activePane(), below);

        window->closeActivePane(); // inner splitter collapses
        QCOMPARE(tab->paneCount(), 2);
        QCOMPARE(qobject_cast<QSplitter*>(right->parentWidget()), splitter);
        QTRY_VERIFY(splitter->count() == 2);

        window->action(Action::ClosePane)->trigger();
        window->action(Action::ClosePane)->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!window->isVisible(), 5000); // last pane closed the window
    }

    void splitStartsInTheShellsDirectory() {
        QTemporaryDir dir;
        QTemporaryDir work;
        const QString script = QStringLiteral("printf '\\033]7;file://localhost%1\\007'; exec sleep 30").arg(work.path());
        auto window = startWindow(makeContext(dir.path(), this), script);
        QVERIFY(window);
        QTRY_COMPARE_WITH_TIMEOUT(QString::fromStdString(window->session()->terminal().workingDirectory()),
                                  work.path(), 5000);
        TerminalPane* pane = window->splitActive(Qt::Horizontal);
        QTRY_VERIFY_WITH_TIMEOUT(pane->session()->isRunning(), 5000);
        pane->session()->sendBytes("pwd\r");
        QTRY_VERIFY_WITH_TIMEOUT(screenContains(*pane->session(), work.path().toUtf8().constData()), 5000);
    }

    void searchHighlightsAndSteps() {
        QTemporaryDir dir;
        auto window = startWindow(makeContext(dir.path(), this),
                                  QStringLiteral("i=0; while [ $i -lt 60 ]; do echo \"line $i needle\"; i=$((i+1)); "
                                                 "done; echo NEEDLE end; exec sleep 30"));
        QVERIFY(window);
        TerminalPane* pane = window->activePane();
        QTRY_VERIFY_WITH_TIMEOUT(screenContains(*pane->session(), "NEEDLE end"), 5000);
        window->action(Action::Find)->trigger();
        SearchBar* bar = pane->searchBar();
        QVERIFY(bar->isVisible());
        bar->setQuery(QStringLiteral("needle"));
        QCOMPARE(bar->matchCount(), 61); // case-insensitive by default
        QCOMPARE(bar->currentIndex(), 60);
        bar->findPrevious();
        QCOMPARE(bar->currentIndex(), 59);
        bar->findNext();
        bar->findNext(); // wraps
        QCOMPARE(bar->currentIndex(), 0);
        QVERIFY(!pane->terminal()->isScrolledToBottom()); // scrolled to the first match in history

        bar->setOptions(true, false, false);
        QCOMPARE(bar->matchCount(), 60);
        bar->setOptions(false, false, true);
        bar->setQuery(QStringLiteral("line [0-9]$"));
        QCOMPARE(bar->matchCount(), 0); // "$" anchors at the end of the logical line
        bar->setQuery(QStringLiteral("line 5[0-9] needle"));
        QCOMPARE(bar->matchCount(), 10);
        bar->setQuery(QStringLiteral("("));
        QCOMPARE(bar->matchCount(), 0);
        QVERIFY(!bar->errorText().isEmpty());

        // New output is picked up while the bar is open.
        bar->setOptions(false, false, false);
        bar->setQuery(QStringLiteral("late-arrival"));
        QCOMPARE(bar->matchCount(), 0);
        pane->session()->terminal().feed("late-arrival\r\n");
        Q_EMIT pane->session()->outputReceived();
        QTRY_COMPARE_WITH_TIMEOUT(bar->matchCount(), 1, 3000);

        // Escape closes the bar and clears the highlights.
        QTest::keyClick(bar->findChild<QLineEdit*>(), Qt::Key_Escape);
        QVERIFY(!bar->isVisible());
        QVERIFY(pane->terminal()->searchResults().empty());
    }

    void themesApplyToAllPanes() {
        QTemporaryDir dir;
        AppContext* context = makeContext(dir.path(), this);
        auto window = startWindow(context, kIdleShell);
        QVERIFY(window);
        TerminalPane* second = window->splitActive(Qt::Horizontal);
        Settings settings = context->settings();
        settings.terminal.theme = "Solarized Light";
        QVERIFY(context->setSettings(settings));
        const Theme theme = *findTheme("Solarized Light");
        for (TerminalPane* pane : window->currentTab()->panes()) {
            QCOMPARE(pane->session()->terminal().palette().background, theme.palette.background);
        }
        QTRY_VERIFY_WITH_TIMEOUT(second->session()->isRunning(), 5000);
        const QImage image = second->terminal()->viewport()->grab().toImage();
        const QPoint probe = cellCenter(*second->terminal(), 10, 5) * image.devicePixelRatio();
        QCOMPARE(image.pixelColor(probe), QColor(0xFD, 0xF6, 0xE3));

        // A session profile can override the theme.
        SessionProfile profile;
        profile.name = "themed";
        profile.kind = SessionKind::Local;
        profile.local.program = "/bin/sh";
        profile.local.arguments = {"-c", "exec sleep 30"};
        profile.theme = "Dracula";
        TerminalPane* themed = window->openProfile(profile);
        QCOMPARE(themed->session()->terminal().palette().background, 0x282A36u);
        QCOMPARE(window->tabs()->tabText(window->tabs()->currentIndex()), QStringLiteral("themed"));
    }

    void keymapDrivesShortcuts() {
        QTemporaryDir dir;
        AppContext* context = makeContext(dir.path(), this);
        Settings settings = context->settings();
        settings.keymap.setSequences(Action::NewTab, {"Alt+Shift+N"});
        context->setSettings(settings);
        auto window = startWindow(context, kIdleShell);
        QVERIFY(window);
        QVERIFY(QTest::qWaitForWindowExposed(window.get()));
        QCOMPARE(window->action(Action::NewTab)->shortcut(), QKeySequence(QStringLiteral("Alt+Shift+N")));
        window->activateWindow();
        window->terminalWidget()->setFocus();
        QTest::keyClick(window->terminalWidget(), Qt::Key_N, Qt::AltModifier | Qt::ShiftModifier);
        QTRY_COMPARE_WITH_TIMEOUT(window->tabCount(), 2, 3000);

        // Re-binding at runtime takes effect immediately.
        settings.keymap.setSequences(Action::NewTab, {});
        context->setSettings(settings);
        QVERIFY(window->action(Action::NewTab)->shortcuts().isEmpty());
    }

    void zoomChangesCellSize() {
        QTemporaryDir dir;
        auto window = startWindow(makeContext(dir.path(), this), kIdleShell);
        QVERIFY(window);
        TerminalWidget* view = window->terminalWidget();
        const QSize base = view->cellSize();
        window->action(Action::ZoomIn)->trigger();
        window->action(Action::ZoomIn)->trigger();
        QVERIFY(view->cellSize().height() > base.height());
        QCOMPARE(view->zoomSteps(), 2);
        window->action(Action::ZoomReset)->trigger();
        QCOMPARE(view->cellSize(), base);
        window->action(Action::ZoomOut)->trigger();
        QVERIFY(view->cellSize().height() <= base.height());
    }

    void hyperlinksAreDetectedAndActivated() {
        TerminalSession session;
        TerminalWidget widget;
        widget.setOpenLinksExternally(false);
        QVERIFY(session.start(shellOptions(QStringLiteral(
            "printf 'see https://example.com/a_(b). and \\033]8;;https://osc8.example/x\\033\\\\label\\033]8;;\\033\\\\ end'; "
            "exec sleep 30"))));
        widget.setSession(&session);
        widget.resize(widget.sizeForGrid(10, 80));
        widget.show();
        QTRY_VERIFY_WITH_TIMEOUT(screenContains(session, "end"), 5000);
        QCOMPARE(widget.linkAt(cellCenter(widget, 0, 10)), QStringLiteral("https://example.com/a_(b)"));
        QCOMPARE(widget.linkAt(cellCenter(widget, 0, 2)), QString());
        const int label = static_cast<int>(session.terminal().lineText(0).find("label"));
        QCOMPARE(widget.linkAt(cellCenter(widget, 0, label + 2)), QStringLiteral("https://osc8.example/x"));

        QSignalSpy activated(&widget, &TerminalWidget::linkActivated);
        sendMouse(widget, QEvent::MouseButtonPress, Qt::LeftButton, Qt::LeftButton, cellCenter(widget, 0, label),
                  Qt::ControlModifier);
        QCOMPARE(activated.count(), 1);
        QCOMPARE(activated.at(0).at(0).toUrl(), QUrl(QStringLiteral("https://osc8.example/x")));
        QVERIFY(!session.terminal().hasSelection());
        session.terminate();
    }

    void textRunsStayOnTheCellGrid() {
        // 79 'W's in one run: if glyphs advanced less than a cell the run would end several cells
        // early and the last cells would stay empty.
        TerminalSession session;
        TerminalWidget widget;
        QVERIFY(session.start(shellOptions(QStringLiteral("printf '%s' \"$(printf 'W%.0s' $(seq 1 79))\"; exec sleep 30"),
                                           5, 80)));
        widget.setSession(&session);
        widget.resize(widget.sizeForGrid(5, 80));
        widget.show();
        QTRY_VERIFY_WITH_TIMEOUT(session.terminal().lineText(0).size() == 79u, 5000);
        const QImage image = widget.viewport()->grab().toImage();
        const QSize cell = widget.cellSize();
        const qreal ratio = image.devicePixelRatio();
        const QColor background = QColor::fromRgb(session.terminal().palette().background);
        auto inkInCell = [&](int column) {
            int ink = 0;
            for (int x = column * cell.width(); x < (column + 1) * cell.width(); ++x) {
                for (int y = 0; y < cell.height(); ++y) {
                    ink += image.pixelColor(QPoint(x, y) * ratio) != background ? 1 : 0;
                }
            }
            return ink;
        };
        QVERIFY(inkInCell(0) > 0);
        QVERIFY(inkInCell(77) > 0);
        QVERIFY(inkInCell(78) > 0);
        session.terminate();
    }

    void clipboardWriteRequiresPermissionAndCopyOnSelect() {
        TerminalSession session;
        TerminalWidget widget;
        TerminalAppearance appearance = widget.appearance();
        appearance.allowClipboardWrite = true;
        appearance.copyOnSelect = true;
        widget.setAppearance(appearance);
        QApplication::clipboard()->clear();
        QVERIFY(session.start(shellOptions(QStringLiteral("printf '\\033]52;c;b3NjNTI=\\007copy me'; exec sleep 30"))));
        widget.setSession(&session);
        widget.show();
        QTRY_COMPARE_WITH_TIMEOUT(QApplication::clipboard()->text(), QStringLiteral("osc52"), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(screenContains(session, "copy me"), 5000);
        sendMouse(widget, QEvent::MouseButtonPress, Qt::LeftButton, Qt::LeftButton, cellCenter(widget, 0, 0));
        sendMouse(widget, QEvent::MouseMove, Qt::NoButton, Qt::LeftButton, cellCenter(widget, 0, 3));
        sendMouse(widget, QEvent::MouseButtonRelease, Qt::LeftButton, Qt::NoButton, cellCenter(widget, 0, 3));
        QCOMPARE(QApplication::clipboard()->text(), QStringLiteral("copy"));
        session.terminate();

        TerminalSession denied;
        TerminalWidget plain;
        QVERIFY(denied.start(shellOptions(QStringLiteral("printf '\\033]52;c;bm8=\\007done'; exec sleep 30"))));
        plain.setSession(&denied);
        QTRY_VERIFY_WITH_TIMEOUT(screenContains(denied, "done"), 5000);
        QCOMPARE(QApplication::clipboard()->text(), QStringLiteral("copy"));
        denied.terminate();
    }

    void settingsPersistAcrossContexts() {
        QTemporaryDir dir;
        {
            AppContext context(dir.path());
            Settings settings = context.settings();
            settings.terminal.theme = "Nord";
            settings.terminal.scrollbackLines = 1234;
            settings.keymap.setSequences(Action::Find, {"Ctrl+Alt+F"});
            QVERIFY(context.setSettings(settings));
            SessionProfile profile;
            profile.name = "box";
            profile.kind = SessionKind::Ssh;
            profile.ssh.host = "box.example";
            profile.group = "Lab";
            context.sessions().upsert(profile);
            QVERIFY(context.saveSessions());
        }
        AppContext reloaded(dir.path());
        QVERIFY(reloaded.warnings().isEmpty());
        QCOMPARE(reloaded.settings().terminal.theme, std::string("Nord"));
        QCOMPARE(reloaded.settings().terminal.scrollbackLines, 1234);
        QCOMPARE(reloaded.settings().keymap.sequences(Action::Find).front(), std::string("Ctrl+Alt+F"));
        QCOMPARE(reloaded.sessions().profiles().size(), std::size_t(1));

        // The scrollback limit reaches the terminals.
        MainWindow::Options options;
        options.session = shellOptions(kIdleShell);
        MainWindow window(options, &reloaded);
        QCOMPARE(window.session()->terminal().scrollbackLimit(), std::size_t(1234));

        // A broken file is reported, not fatal.
        QVERIFY(wt::platform::writeFileAtomic(reloaded.settingsPath().toStdString(), "{oops").isOk());
        AppContext broken(dir.path());
        QVERIFY(!broken.warnings().isEmpty());
        QCOMPARE(broken.settings().terminal.theme, std::string(kDefaultThemeName));
    }

    void savedLocalProfileOpensFromCommandLineOption() {
        QTemporaryDir dir;
        AppContext* context = makeContext(dir.path(), this);
        SessionProfile profile;
        profile.name = "greeter";
        profile.kind = SessionKind::Local;
        profile.local.program = "/bin/sh";
        profile.local.arguments = {"-c", "echo \"hello $GREETING\"; exec sleep 30"};
        profile.local.environment = {{"GREETING", "profile"}};
        const std::string id = context->sessions().upsert(profile);
        MainWindow::Options options;
        options.profileId = QString::fromStdString(id);
        MainWindow window(options, context);
        QVERIFY2(window.start(), qPrintable(window.errorString()));
        window.show();
        QTRY_VERIFY_WITH_TIMEOUT(screenContains(*window.session(), "hello profile"), 5000);

        MainWindow::Options missing;
        missing.profileId = QStringLiteral("nope");
        MainWindow other(missing, context);
        QVERIFY(!other.start());
        QVERIFY(other.errorString().contains(QStringLiteral("nope")));
    }

    void settingsDialogRoundTripsAndValidatesKeys() {
        Settings settings;
        settings.terminal.theme = "Dracula";
        settings.terminal.scrollbackLines = 777;
        SettingsDialog dialog(settings);
        QCOMPARE(dialog.settings().terminal.theme, std::string("Dracula"));
        QCOMPARE(dialog.settings().terminal.scrollbackLines, 777);
        QVERIFY(dialog.keymapProblems().isEmpty());
        auto* table = dialog.findChild<QTableWidget*>(QStringLiteral("keybindings"));
        QVERIFY(table);
        // Bind "Copy" to the paste shortcut: a conflict is reported.
        for (int row = 0; row < table->rowCount(); ++row) {
            if (table->item(row, 0)->data(Qt::UserRole).toInt() == static_cast<int>(Action::Copy)) {
                table->item(row, 1)->setText(QString::fromStdString(settings.keymap.sequences(Action::Paste).front()));
            }
        }
        QCOMPARE(dialog.keymapProblems().size(), 1);
    }

    // Renders a populated window (tabs, a split, the find bar, colours); saved to
    // $WT_TEST_SCREENSHOT_DIR/workspace.png for visual inspection when that variable is set.
    void windowRendersPopulatedWorkspace() {
        QTemporaryDir dir;
        auto window = startWindow(makeContext(dir.path(), this),
                                  QStringLiteral("printf '\\033[1;32muser@host\\033[0m:\\033[34m~/src\\033[0m$ ls\\n';"
                                                 "printf '\\033[4:3;58:2::255:0:0mcurly\\033[0m \\033[53moverline\\033[0m "
                                                 "\\033]8;;https://example.com\\033\\\\link\\033]8;;\\033\\\\ needle\\n'; "
                                                 "exec sleep 30"));
        QVERIFY(window);
        window->resize(1000, 600);
        window->newTab();
        window->tabs()->setCurrentIndex(0);
        TerminalPane* first = window->activePane();
        QTRY_VERIFY_WITH_TIMEOUT(screenContains(*first->session(), "needle"), 5000);
        window->splitActive(Qt::Horizontal);
        first->terminal()->setFocus();
        first->showSearch();
        first->searchBar()->setQuery(QStringLiteral("needle"));
        QCOMPARE(first->searchBar()->matchCount(), 1);
        QTest::qWait(100);
        const QImage image = window->grab().toImage();
        QVERIFY(!image.isNull());
        if (const QByteArray out = qgetenv("WT_TEST_SCREENSHOT_DIR"); !out.isEmpty()) {
            QVERIFY(image.save(QString::fromLocal8Bit(out) + QStringLiteral("/workspace.png")));
        }
    }

    void sessionManagerListsGroupsAndEdits() {
        QTemporaryDir dir;
        AppContext* context = makeContext(dir.path(), this);
        SessionProfile a;
        a.name = "alpha";
        a.kind = SessionKind::Ssh;
        a.ssh.host = "alpha.example";
        a.group = "Prod/EU";
        SessionProfile b;
        b.name = "beta";
        b.kind = SessionKind::Local;
        const std::string idA = context->sessions().upsert(a);
        context->sessions().upsert(b);
        SessionManagerDialog dialog(context);
        QTreeWidget* tree = dialog.tree();
        QCOMPARE(tree->topLevelItemCount(), 2); // "Prod" folder + beta
        dialog.selectId(QString::fromStdString(idA));
        QCOMPARE(dialog.selectedId(), QString::fromStdString(idA));
        QCOMPARE(tree->currentItem()->parent()->text(0), QStringLiteral("EU"));

        SessionEditDialog edit(context, *context->sessions().find(idA));
        QCOMPARE(edit.profile(), *context->sessions().find(idA));
        edit.findChild<QLineEdit*>(QStringLiteral("host"))->clear();
        QVERIFY(!edit.problems().isEmpty());
    }
};

QTEST_MAIN(WorkspaceTests)
#include "WorkspaceTests.moc"
