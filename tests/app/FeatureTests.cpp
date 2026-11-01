// GUI tests for session logs, triggers, command completion and input broadcasting, with local
// /bin/sh sessions.

#include "AppContext.h"
#include "MainWindow.h"
#include "SettingsDialog.h"
#include "TerminalPane.h"
#include "TerminalSession.h"
#include "TerminalTab.h"
#include "TerminalWidget.h"
#include "Translations.h"

#include "wt/platform/Files.h"

#include <QMenuBar>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QTest>

#include <memory>

using namespace wt::session;

namespace {

bool screenContains(const TerminalSession& session, const QString& text) {
    return QString::fromStdString(session.terminal().screenText()).contains(text);
}

int countOnScreen(const TerminalSession& session, const QString& text) {
    return static_cast<int>(QString::fromStdString(session.terminal().screenText()).count(text));
}

// The absolute row whose text contains `text`, or -1.
std::int64_t rowWith(const TerminalSession& session, const std::string& text) {
    const auto& t = session.terminal();
    for (int row = 0; row < t.rows(); ++row) {
        if (t.lineText(row).find(text) != std::string::npos) {
            return t.screenStart() + row;
        }
    }
    return -1;
}

} // namespace

class FeatureTests : public QObject {
    Q_OBJECT

    QTemporaryDir m_dir;

    AppContext* makeContext(const std::function<void(Settings&)>& adjust = {}) {
        static int counter = 0;
        auto* context = new AppContext(m_dir.filePath(QStringLiteral("config-%1").arg(++counter)), this);
        Settings settings = context->settings();
        settings.confirmCloseRunning = false;
        settings.logging.directory = m_dir.filePath(QStringLiteral("logs")).toStdString();
        if (adjust) {
            adjust(settings);
        }
        context->setSettings(settings);
        return context;
    }

    std::unique_ptr<MainWindow> window(AppContext* context, const QString& script = {}) {
        MainWindow::Options options;
        options.session.program = QStringLiteral("/bin/sh");
        if (!script.isEmpty()) {
            options.session.arguments = {QStringLiteral("-c"), script};
        }
        auto w = std::make_unique<MainWindow>(options, context);
        if (!w->start()) {
            return nullptr;
        }
        w->show();
        w->activateWindow();
        return w;
    }

private Q_SLOTS:
    void initTestCase() {
        QStandardPaths::setTestModeEnabled(true);
        qputenv("SHELL", "/bin/sh"); // new tabs and splits run a predictable shell
        qputenv("ENV", "");
        qputenv("PS1", "$ ");
    }

    void sessionLogWritesPlainText() {
        AppContext* context = makeContext([](Settings& s) {
            s.logging.autoStart = true;
            s.logging.timestamps = false;
            s.logging.fileName = "%N.log";
        });
        auto w = window(context, QStringLiteral("sleep 0.3; printf 'hello \\033[31mred\\033[0m\\r\\nprogress 10%%\\rprogress 100%%\\n'; "
                                                "exec sleep 30"));
        QVERIFY(w);
        TerminalPane* pane = w->activePane();
        QTRY_VERIFY_WITH_TIMEOUT(pane->isLogging(), 5000);
        QVERIFY(w->action(Action::ToggleSessionLog)->isChecked());
        const QString path = pane->session()->logPath();
        QVERIFY(path.startsWith(m_dir.filePath(QStringLiteral("logs"))));
        QTRY_VERIFY_WITH_TIMEOUT(screenContains(*pane->session(), QStringLiteral("progress 100%")), 5000);
        // Turning it off from the menu action closes the file.
        w->action(Action::ToggleSessionLog)->trigger();
        QVERIFY(!pane->isLogging());
        QVERIFY(!w->action(Action::ToggleSessionLog)->isChecked());
        const auto contents = wt::platform::readFile(path.toStdString());
        QVERIFY(contents.has_value());
        QCOMPARE(QString::fromStdString(*contents), QStringLiteral("hello red\nprogress 100%\n"));
        // And on again: a second file next to the first.
        w->action(Action::ToggleSessionLog)->trigger();
        QVERIFY(pane->isLogging());
        QVERIFY(pane->session()->logPath() != path);
    }

    void triggersHighlightNotifyAndAnswer() {
        AppContext* context = makeContext([](Settings& s) {
            s.triggers = {{"errors", "ERROR|FATAL", true, true, TriggerAction::Highlight, "#ffffff", "#aa0000", ""},
                          {"deploy", "deploy (done|ok)", false, true, TriggerAction::Notify, "", "", ""},
                          {"password", "^Password: *$", false, true, TriggerAction::Send, "", "", "s3cret\\r"},
                          {"disabled", "deploy", false, false, TriggerAction::Notify, "", "", "never"}};
        });
        QVERIFY(context->triggerErrors().isEmpty());
        auto w = window(context, QStringLiteral("printf 'Password: '; read -r pw; echo \"got:$pw\"; "
                                                "echo 'an ERROR here and FATAL there'; sleep 0.2; echo 'Deploy DONE'; exec sleep 30"));
        QVERIFY(w);
        TerminalPane* pane = w->activePane();
        QSignalSpy notified(pane, &TerminalPane::triggerNotification);
        // The send trigger answered the prompt (before any newline arrived).
        QTRY_VERIFY_WITH_TIMEOUT(screenContains(*pane->session(), QStringLiteral("got:s3cret")), 5000);
        QTRY_COMPARE_WITH_TIMEOUT(notified.count(), 1, 5000);
        QCOMPARE(notified.first().at(1).toString(), QStringLiteral("deploy"));
        QVERIFY(notified.first().at(2).toString().contains(QStringLiteral("Deploy DONE")));
        QTRY_VERIFY_WITH_TIMEOUT(w->statusBar()->currentMessage().contains(QStringLiteral("Deploy DONE")), 2000);
        // Highlights: both words on their row, case-sensitively.
        const std::int64_t row = rowWith(*pane->session(), "an ERROR here");
        QVERIFY(row >= 0);
        const auto matches = pane->terminal()->highlightMatches(row);
        QCOMPARE(matches.size(), std::size_t(2));
        QCOMPARE(matches[0].startColumn, 3);
        QCOMPARE(matches[0].endColumn, 7);
        QCOMPARE(matches[1].startColumn, 18);
        QCOMPARE(matches[1].endColumn, 22);
        QVERIFY(pane->terminal()->highlightMatches(rowWith(*pane->session(), "Deploy DONE")).empty());
        // Painting with highlights works (and is exercised under the sanitizers).
        QImage image(pane->terminal()->viewport()->size(), QImage::Format_ARGB32);
        pane->terminal()->viewport()->render(&image);
    }

    void completionSuggestsAndInsertsFromHistory() {
        AppContext* context = makeContext([](Settings& s) { s.completion.minimumPrefix = 2; });
        context->recordCommand(QStringLiteral("local"), QStringLiteral("echo completion-$((40+2))"));
        context->recordCommand(QStringLiteral("other-host"), QStringLiteral("echo elsewhere"));
        auto w = window(context); // interactive /bin/sh
        QVERIFY(w);
        TerminalPane* pane = w->activePane();
        TerminalWidget* view = pane->terminal();
        view->setFocus();
        QTRY_VERIFY_WITH_TIMEOUT(screenContains(*pane->session(), QStringLiteral("$")), 5000);
        QTest::keyClicks(view, QStringLiteral("ec"));
        QTRY_VERIFY_WITH_TIMEOUT(view->completionVisible(), 2000);
        QCOMPARE(view->completionItems(),
                 QStringList({QStringLiteral("echo completion-$((40+2))"), QStringLiteral("echo elsewhere")}));
        // Enter without choosing runs what was typed; choosing first inserts the suggestion.
        QTest::keyClick(view, Qt::Key_Down);
        QTest::keyClick(view, Qt::Key_Return);
        QVERIFY(!view->completionVisible());
        QCOMPARE(view->typedLine(), QStringLiteral("echo completion-$((40+2))"));
        QTest::keyClick(view, Qt::Key_Return);
        QTRY_VERIFY_WITH_TIMEOUT(screenContains(*pane->session(), QStringLiteral("completion-42")), 5000);

        // New commands are remembered once the shell has echoed them.
        QTest::keyClicks(view, QStringLiteral("echo fresh-command"));
        QVERIFY(!view->completionVisible()); // nothing in the history starts like that
        QTest::keyClick(view, Qt::Key_Return);
        QTRY_VERIFY_WITH_TIMEOUT(context->completeCommand(QStringLiteral("local"), QStringLiteral("echo f"))
                                     .contains(QStringLiteral("echo fresh-command")),
                                 3000);

        // Input at a prompt that does not echo (a password) is never recorded.
        QTest::keyClicks(view, QStringLiteral("stty -echo; read -r secret; stty echo; echo read-done"));
        QTest::keyClick(view, Qt::Key_Return);
        QTest::qWait(200);
        QTest::keyClicks(view, QStringLiteral("topsecret-value"));
        QTest::keyClick(view, Qt::Key_Return);
        QTRY_VERIFY_WITH_TIMEOUT(screenContains(*pane->session(), QStringLiteral("read-done")), 5000);
        QTest::qWait(500);
        QVERIFY(context->completeCommand(QStringLiteral("local"), QStringLiteral("top")).isEmpty());
        QVERIFY(!screenContains(*pane->session(), QStringLiteral("topsecret")));

        // Full-screen programs (alternate screen) get no suggestions.
        pane->session()->terminal().feed("\x1b[?1049h");
        QTest::keyClicks(view, QStringLiteral("ec"));
        QVERIFY(!view->completionVisible());
        pane->session()->terminal().feed("\x1b[?1049l");
    }

    void broadcastInputToTabAndWindow() {
        AppContext* context = makeContext();
        auto w = window(context);
        QVERIFY(w);
        TerminalPane* first = w->activePane();
        TerminalPane* second = w->splitActive(Qt::Horizontal);
        QVERIFY(second && second != first);
        w->newTab();
        TerminalPane* other = w->activePane();
        for (TerminalPane* pane : {first, second, other}) {
            QTRY_VERIFY_WITH_TIMEOUT(screenContains(*pane->session(), QStringLiteral("$")), 5000);
        }
        w->tabs()->setCurrentIndex(0);
        w->action(Action::BroadcastToTab)->trigger();
        QCOMPARE(w->broadcast(), MainWindow::Broadcast::Tab);
        QTRY_VERIFY(w->findChild<QWidget*>(QStringLiteral("broadcastIndicator"))->isVisible());
        QTest::keyClicks(first->terminal(), QStringLiteral("echo tab-$((6*7))"));
        QTest::keyClick(first->terminal(), Qt::Key_Return);
        QTRY_VERIFY_WITH_TIMEOUT(screenContains(*first->session(), QStringLiteral("tab-42")), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(screenContains(*second->session(), QStringLiteral("tab-42")), 5000);
        QTest::qWait(200);
        QVERIFY(!screenContains(*other->session(), QStringLiteral("tab-42"))); // another tab

        w->action(Action::BroadcastToAll)->trigger();
        QCOMPARE(w->broadcast(), MainWindow::Broadcast::All);
        QVERIFY(!w->action(Action::BroadcastToTab)->isChecked());
        QTest::keyClicks(second->terminal(), QStringLiteral("echo all-$((7*6))"));
        QTest::keyClick(second->terminal(), Qt::Key_Return);
        for (TerminalPane* pane : {first, second, other}) {
            QTRY_VERIFY_WITH_TIMEOUT(screenContains(*pane->session(), QStringLiteral("all-42")), 5000);
        }

        w->action(Action::BroadcastToAll)->trigger(); // off again
        QCOMPARE(w->broadcast(), MainWindow::Broadcast::Off);
        QTest::keyClicks(first->terminal(), QStringLiteral("echo solo-1"));
        QTest::keyClick(first->terminal(), Qt::Key_Return);
        QTRY_VERIFY_WITH_TIMEOUT(screenContains(*first->session(), QStringLiteral("solo-1")), 5000);
        QTest::qWait(200);
        QCOMPARE(countOnScreen(*second->session(), QStringLiteral("solo-1")), 0);
    }

    void kittyReleaseAndModifierEventsReachTheProgram() {
        // flags 11 = disambiguate | report events | report all keys
        auto w = window(makeContext(), QStringLiteral("stty -icanon -echo; printf '\\033[>11uREADY\\n'; printf 'got:'; "
                                                      "head -c 44 | cat -v; echo; exec sleep 30"));
        QVERIFY(w);
        TerminalPane* pane = w->activePane();
        TerminalWidget* view = pane->terminal();
        view->setFocus();
        QTRY_VERIFY_WITH_TIMEOUT(screenContains(*pane->session(), QStringLiteral("READY")), 5000);
        QCOMPARE(pane->session()->terminal().kittyKeyboardFlags(), 11u);
        QTest::keyPress(view, Qt::Key_A);                         // CSI 97 u
        QTest::keyRelease(view, Qt::Key_A);                       // CSI 97 ; 1:3 u
        QTest::keyPress(view, Qt::Key_1, Qt::KeypadModifier);     // CSI 57400 u (KP_1)
        QTest::keyRelease(view, Qt::Key_1, Qt::KeypadModifier);   // CSI 57400 ; 1:3 u
        QTest::keyPress(view, Qt::Key_Shift, Qt::ShiftModifier);  // CSI 57441 ; 2 u
        QTRY_VERIFY2_WITH_TIMEOUT(screenContains(*pane->session(),
                                                 QStringLiteral("got:^[[97u^[[97;1:3u^[[57400u^[[57400;1:3u^[[57441;2u")),
                                  pane->session()->terminal().screenText().c_str(), 5000);
    }

    void chineseTranslationIsComplete() {
        QCOMPARE(Translations::install(qApp, QStringLiteral("zh_CN")), QStringLiteral("zh_CN"));
        {
            AppContext* context = makeContext();
            auto w = window(context, QStringLiteral("exec sleep 30"));
            QVERIFY(w);
            QStringList menus;
            for (QAction* action : w->menuBar()->actions()) {
                menus << action->text();
            }
            QVERIFY2(menus.contains(QStringLiteral("文件(&F)")), qPrintable(menus.join(QLatin1Char(','))));
            QVERIFY(menus.contains(QStringLiteral("会话(&S)")));
            QCOMPARE(w->action(Action::NewTab)->text(), QStringLiteral("新建标签页"));
            QCOMPARE(w->action(Action::BroadcastToAll)->text(), QStringLiteral("广播输入到所有标签页"));
            SettingsDialog dialog(context->settings());
            QCOMPARE(dialog.windowTitle(), QStringLiteral("设置"));
            QCOMPARE(QCoreApplication::translate("MainWindow", "%n session(s) are still running. Close the window anyway?",
                                                 nullptr, 3),
                     QStringLiteral("仍有 3 个会话正在运行。仍要关闭窗口吗？"));
        }
        // Every action title has a translation (titles come from the Qt-free keymap).
        for (const auto& info : allActions()) {
            QVERIFY2(Translations::actionTitle(info.title.data()) != QString::fromUtf8(info.title.data()),
                     info.title.data());
        }
        Translations::uninstall(qApp);
        QCOMPARE(Translations::actionTitle("New Tab"), QStringLiteral("New Tab"));
        QCOMPARE(Translations::install(qApp, QStringLiteral("en")), QStringLiteral("en"));
        QCOMPARE(Translations::install(qApp, QStringLiteral("xx_YY")), QStringLiteral("en")); // unknown: English
        Translations::uninstall(qApp);
    }

    void settingsDialogEditsLoggingAndTriggers() {
        Settings settings;
        settings.logging.autoStart = true;
        settings.logging.format = LogFormat::Raw;
        settings.triggers = {{"t", "x+", false, true, TriggerAction::Beep, "", "", ""}};
        settings.completion.minimumPrefix = 4;
        SettingsDialog dialog(settings);
        QCOMPARE(dialog.settings(), settings);
        dialog.setTriggers({});
        QVERIFY(dialog.settings().triggers.empty());
    }
};

QTEST_MAIN(FeatureTests)
#include "FeatureTests.moc"
