// GUI-level integration tests. Run with QT_QPA_PLATFORM=offscreen (set by CTest).

#include "MainWindow.h"
#include "TerminalSession.h"
#include "TerminalWidget.h"

#include <QApplication>
#include <QBuffer>
#include <QClipboard>
#include <QElapsedTimer>
#include <QImage>
#include <QMouseEvent>
#include <QScrollBar>
#include <QSignalSpy>
#include <QTest>
#include <QWheelEvent>

using wt::terminal::Color;

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

#ifdef Q_OS_MACOS
constexpr Qt::KeyboardModifier kTerminalControl = Qt::MetaModifier;
const Qt::KeyboardModifiers kCopyModifiers = Qt::ControlModifier; // Command
#else
constexpr Qt::KeyboardModifier kTerminalControl = Qt::ControlModifier;
const Qt::KeyboardModifiers kCopyModifiers = Qt::ControlModifier | Qt::ShiftModifier;
#endif

// Centre of a cell in viewport coordinates.
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

void drag(TerminalWidget& widget, const QPoint& from, const QPoint& to, Qt::KeyboardModifiers modifiers = {}) {
    sendMouse(widget, QEvent::MouseButtonPress, Qt::LeftButton, Qt::LeftButton, from, modifiers);
    sendMouse(widget, QEvent::MouseMove, Qt::NoButton, Qt::LeftButton, to, modifiers);
    sendMouse(widget, QEvent::MouseButtonRelease, Qt::LeftButton, Qt::NoButton, to, modifiers);
}

void sendWheel(TerminalWidget& widget, const QPoint& pos, int delta) {
    QWidget* viewport = widget.viewport();
    QWheelEvent event(QPointF(pos), QPointF(viewport->mapToGlobal(pos)), QPoint(), QPoint(0, delta), Qt::NoButton,
                      Qt::NoModifier, Qt::NoScrollPhase, false);
    QApplication::sendEvent(viewport, &event);
}

// A child that switches the tty to non-canonical mode, enables some terminal modes and prints
// what it receives next (in `cat -v` notation) after "got:".
QString echoInputScript(const QString& setup, int bytes) {
    return QStringLiteral("stty -icanon -echo; printf '%1READY\\n'; printf 'got:'; head -c %2 | cat -v; echo; "
                          "sleep 0.3")
        .arg(setup)
        .arg(bytes);
}

} // namespace

class AppTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void sessionRendersOutputAndReportsExit() {
        TerminalSession session;
        QSignalSpy finished(&session, &TerminalSession::finished);
        QVERIFY2(session.start(shellOptions(QStringLiteral("printf 'hello \\033[31mred\\033[0m'; exit 4"))),
                 qPrintable(session.errorString()));
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 5000);
        QCOMPARE(finished.at(0).at(0).toInt(), 4);
        QCOMPARE(session.terminal().lineText(0), std::string("hello red"));
        QVERIFY(session.terminal().line(0).cells[6].attributes.foreground == Color::indexed(1));
        QVERIFY(!session.isRunning());
    }

    void sessionReportsStartFailure() {
        TerminalSession session;
        TerminalSession::Options options;
        options.program = QStringLiteral("/nonexistent/shell");
        QVERIFY(!session.start(options));
        QVERIFY(session.errorString().contains(QStringLiteral("not found")));
    }

    void titleSequenceUpdatesSession() {
        TerminalSession session;
        QSignalSpy titles(&session, &TerminalSession::titleChanged);
        QVERIFY(session.start(shellOptions(QStringLiteral("printf '\\033]0;My Title\\007'; sleep 0.2"))));
        QTRY_COMPARE_WITH_TIMEOUT(titles.count(), 1, 5000);
        QCOMPARE(titles.at(0).at(0).toString(), QStringLiteral("My Title"));
    }

    void keyboardInputReachesChild() {
        TerminalSession session;
        TerminalWidget widget;
        QVERIFY(session.start(shellOptions(QStringLiteral("read line; echo \"got:$line\"; sleep 0.2"))));
        widget.setSession(&session);
        widget.resize(widget.sizeHint());
        widget.show();
        QTest::keyClicks(&widget, QStringLiteral("abc"));
        QTest::keyClick(&widget, Qt::Key_Backspace);
        QTest::keyClicks(&widget, QStringLiteral("d"));
        QTest::keyClick(&widget, Qt::Key_Return);
        QTRY_VERIFY_WITH_TIMEOUT(screenContains(session, "got:abd"), 5000);
    }

    void controlCInterruptsForegroundJob() {
        TerminalSession session;
        TerminalWidget widget;
        QVERIFY(session.start(shellOptions(
            QStringLiteral("trap 'echo INTERRUPTED; exit 0' INT; echo READY; while :; do sleep 0.05; done"))));
        widget.setSession(&session);
        widget.show();
        QTRY_VERIFY_WITH_TIMEOUT(screenContains(session, "READY"), 5000);
        QTest::keyClick(&widget, Qt::Key_C, kTerminalControl);
        QTRY_VERIFY_WITH_TIMEOUT(screenContains(session, "INTERRUPTED"), 5000);
    }

    void widgetResizePropagatesToPty() {
        TerminalSession session;
        TerminalWidget widget;
        QVERIFY(session.start(shellOptions(QStringLiteral("read x; stty size; sleep 0.2"))));
        widget.setSession(&session);
        widget.resize(widget.sizeForGrid(30, 100));
        widget.show();
        QTRY_COMPARE_WITH_TIMEOUT(session.terminal().columns(), 100, 2000);
        QCOMPARE(session.terminal().rows(), 30);
        QTest::keyClick(&widget, Qt::Key_Return);
        QTRY_VERIFY_WITH_TIMEOUT(screenContains(session, "30 100"), 5000);
    }

    void widgetPaintsRichContent() {
        TerminalSession session;
        TerminalWidget widget;
        QVERIFY(session.start(shellOptions(QStringLiteral(
            "i=0; while [ $i -lt 60 ]; do echo line$i; i=$((i+1)); done;"
            "printf '\\033[1;44mbold\\033[0m \\033[7minv\\033[0m 中文 \\033[38;2;255;128;0mrgb\\033[4mul\\033[0m\\n';"
            "printf '\\033(0lqqk\\033(B \\033[32mgreen\\033[0m'; sleep 0.3"))));
        widget.setSession(&session);
        widget.resize(widget.sizeForGrid(24, 80));
        widget.show();
        QTRY_VERIFY_WITH_TIMEOUT(screenContains(session, "line59"), 5000);
        QVERIFY(session.terminal().scrollbackSize() > 0);
        QVERIFY(widget.isScrolledToBottom());
        const QImage image = widget.grab().toImage();
        QVERIFY(!image.isNull());
        QCOMPARE(image.size(), widget.size() * image.devicePixelRatio());
        if (const QByteArray dir = qgetenv("WT_TEST_SCREENSHOT_DIR"); !dir.isEmpty()) {
            QVERIFY(image.save(QString::fromLocal8Bit(dir) + QStringLiteral("/terminal-widget.png")));
        }
    }

    void mouseDragSelectsAndCopies() {
        TerminalSession session;
        TerminalWidget widget;
        QVERIFY(session.start(shellOptions(QStringLiteral("printf 'hello world\\nsecond line'; sleep 5"))));
        widget.setSession(&session);
        widget.resize(widget.sizeForGrid(24, 80));
        widget.show();
        QTRY_VERIFY_WITH_TIMEOUT(screenContains(session, "second line"), 5000);

        // A plain click does not select anything.
        drag(widget, cellCenter(widget, 0, 2), cellCenter(widget, 0, 2));
        QVERIFY(!session.terminal().hasSelection());

        drag(widget, cellCenter(widget, 0, 6), cellCenter(widget, 1, 5));
        QCOMPARE(widget.selectedText(), QStringLiteral("world\nsecond"));
        QApplication::clipboard()->clear();
        QTest::keyClick(&widget, Qt::Key_C, kCopyModifiers);
        QCOMPARE(QApplication::clipboard()->text(), QStringLiteral("world\nsecond"));
        QVERIFY(session.isRunning()); // the shortcut did not reach the child as ^C

        // Alt+drag selects a block.
        drag(widget, cellCenter(widget, 0, 1), cellCenter(widget, 1, 3), Qt::AltModifier);
        QCOMPARE(widget.selectedText(), QStringLiteral("ell\neco"));
        session.terminate();
    }

    void doubleAndTripleClickSelectWordAndLine() {
        TerminalSession session;
        TerminalWidget widget;
        QVERIFY(session.start(shellOptions(QStringLiteral("printf 'cd /usr/local/bin now'; sleep 5"))));
        widget.setSession(&session);
        widget.show();
        QTRY_VERIFY_WITH_TIMEOUT(screenContains(session, "now"), 5000);
        const QPoint pos = cellCenter(widget, 0, 8);
        sendMouse(widget, QEvent::MouseButtonPress, Qt::LeftButton, Qt::LeftButton, pos);
        sendMouse(widget, QEvent::MouseButtonRelease, Qt::LeftButton, Qt::NoButton, pos);
        sendMouse(widget, QEvent::MouseButtonDblClick, Qt::LeftButton, Qt::LeftButton, pos);
        sendMouse(widget, QEvent::MouseButtonRelease, Qt::LeftButton, Qt::NoButton, pos);
        QCOMPARE(widget.selectedText(), QStringLiteral("/usr/local/bin"));
        sendMouse(widget, QEvent::MouseButtonPress, Qt::LeftButton, Qt::LeftButton, pos);
        sendMouse(widget, QEvent::MouseButtonRelease, Qt::LeftButton, Qt::NoButton, pos);
        QCOMPARE(widget.selectedText(), QStringLiteral("cd /usr/local/bin now"));
        session.terminate();
    }

    void mouseReportingSendsSgrEvents() {
        TerminalSession session;
        TerminalWidget widget;
        QVERIFY(session.start(shellOptions(echoInputScript(QStringLiteral("\\033[?1000h\\033[?1006h"), 18))));
        widget.setSession(&session);
        widget.show();
        QTRY_VERIFY_WITH_TIMEOUT(screenContains(session, "READY"), 5000);
        QTRY_VERIFY(session.terminal().modes().mouseTracking == wt::terminal::MouseTracking::Normal);
        const QPoint pos = cellCenter(widget, 2, 3);
        sendMouse(widget, QEvent::MouseButtonPress, Qt::LeftButton, Qt::LeftButton, pos);
        sendMouse(widget, QEvent::MouseButtonRelease, Qt::LeftButton, Qt::NoButton, pos);
        QTRY_VERIFY_WITH_TIMEOUT(screenContains(session, "got:^[[<0;4;3M^[[<0;4;3m"), 5000);
        QVERIFY(!session.terminal().hasSelection()); // the click went to the application
    }

    void wheelOnAlternateScreenSendsCursorKeys() {
        TerminalSession session;
        TerminalWidget widget;
        QVERIFY(session.start(shellOptions(echoInputScript(QStringLiteral("\\033[?1049h"), 9))));
        widget.setSession(&session);
        widget.show();
        QTRY_VERIFY_WITH_TIMEOUT(screenContains(session, "READY"), 5000);
        QTRY_VERIFY(session.terminal().isAlternateScreen());
        sendWheel(widget, cellCenter(widget, 1, 1), 120);
        QTRY_VERIFY_WITH_TIMEOUT(screenContains(session, "got:^[[A^[[A^[[A"), 5000);
    }

    void wheelOnPrimaryScreenScrollsHistory() {
        TerminalSession session;
        TerminalWidget widget;
        QVERIFY(session.start(shellOptions(
            QStringLiteral("i=0; while [ $i -lt 100 ]; do echo row$i; i=$((i+1)); done; sleep 5"))));
        widget.setSession(&session);
        widget.resize(widget.sizeForGrid(10, 40));
        widget.show();
        QTRY_VERIFY_WITH_TIMEOUT(screenContains(session, "row99"), 5000);
        QTRY_VERIFY(widget.isScrolledToBottom());
        sendWheel(widget, cellCenter(widget, 1, 1), 120);
        QVERIFY(!widget.isScrolledToBottom());
        session.terminate();
    }

    void keypadApplicationModeSendsSs3() {
        TerminalSession session;
        TerminalWidget widget;
        QVERIFY(session.start(shellOptions(echoInputScript(QStringLiteral("\\033="), 3))));
        widget.setSession(&session);
        widget.show();
        QTRY_VERIFY_WITH_TIMEOUT(screenContains(session, "READY"), 5000);
        QTRY_VERIFY(session.terminal().modes().keypadApplication);
        QTest::keyClick(&widget, Qt::Key_5, Qt::KeypadModifier, 0);
        QTRY_VERIFY_WITH_TIMEOUT(screenContains(session, "got:^[Ou"), 5000);
    }

    void oscBackgroundColorIsPainted() {
        TerminalSession session;
        TerminalWidget widget;
        QVERIFY(session.start(shellOptions(QStringLiteral("printf '\\033]11;#336699\\007ok'; sleep 5"))));
        widget.setSession(&session);
        widget.resize(widget.sizeForGrid(10, 40));
        widget.show();
        QTRY_VERIFY_WITH_TIMEOUT(screenContains(session, "ok"), 5000);
        QCOMPARE(session.terminal().palette().background, 0x336699u);
        const QImage image = widget.viewport()->grab().toImage();
        const QPoint probe = cellCenter(widget, 5, 20) * image.devicePixelRatio();
        QCOMPARE(image.pixelColor(probe), QColor(0x33, 0x66, 0x99));
        session.terminate();
    }

    void sixelImageIsPaintedAndErased() {
        TerminalSession session;
        TerminalWidget widget;
        // A 40 x 42 pixel red sixel image at the top-left, then a marker; later ED 2 removes it.
        QVERIFY(session.start(shellOptions(QStringLiteral(
            "printf '\\033Pq#1;2;100;0;0#1!40~-!40~-!40~-!40~-!40~-!40~-!40~\\033\\\\ok'; read x; "
            "printf '\\033[2Jgone'; sleep 5"))));
        widget.setSession(&session);
        widget.resize(widget.sizeForGrid(10, 40));
        widget.show();
        QTRY_VERIFY_WITH_TIMEOUT(screenContains(session, "ok"), 5000);
        QCOMPARE(session.terminal().imageCount(), std::size_t(1));
        QImage image = widget.viewport()->grab().toImage();
        const QPoint probe = QPoint(6, 6) * image.devicePixelRatio();
        QCOMPARE(image.pixelColor(probe), QColor(0xFF, 0, 0));
        session.sendInput("\r");
        QTRY_VERIFY_WITH_TIMEOUT(screenContains(session, "gone"), 5000);
        QCOMPARE(session.terminal().imageCount(), std::size_t(0));
        image = widget.viewport()->grab().toImage();
        QVERIFY(image.pixelColor(probe) != QColor(0xFF, 0, 0));
        session.terminate();
    }

    void kittyPngImageIsPainted() {
        // A blue PNG sent with the kitty graphics protocol, scaled to 4 x 2 cells.
        QImage png(16, 16, QImage::Format_ARGB32);
        png.fill(QColor(0, 0, 255));
        QByteArray encoded;
        QBuffer buffer(&encoded);
        QVERIFY(buffer.open(QIODevice::WriteOnly) && png.save(&buffer, "PNG"));
        TerminalSession session;
        TerminalWidget widget;
        QVERIFY(session.start(shellOptions(
            QStringLiteral("printf '\\033_Ga=T,f=100,c=4,r=2,q=2;%s\\033\\\\ok' '%1'; sleep 5")
                .arg(QString::fromLatin1(encoded.toBase64())))));
        widget.setSession(&session);
        widget.resize(widget.sizeForGrid(10, 40));
        widget.show();
        QTRY_VERIFY_WITH_TIMEOUT(screenContains(session, "ok"), 5000);
        QCOMPARE(session.terminal().imageCount(), std::size_t(1));
        const auto* placed = session.terminal().visibleImages().front();
        QCOMPARE(placed->columns, 4);
        QCOMPARE(placed->rows, 2);
        const QImage image = widget.viewport()->grab().toImage();
        const QPoint probe = cellCenter(widget, 1, 2) * image.devicePixelRatio();
        QCOMPARE(image.pixelColor(probe), QColor(0, 0, 255));
        session.terminate();
    }

    void closedOutputDoesNotBlockAndHangsUpChild() {
        TerminalSession session;
        QSignalSpy finished(&session, &TerminalSession::finished);
        // The child gives up the terminal (EIO/EOF on the master) but keeps running.
        QVERIFY(session.start(shellOptions(QStringLiteral("exec 0<&- 1>&- 2>&-; exec sleep 30"))));
        int ticks = 0;
        QTimer ticker;
        connect(&ticker, &QTimer::timeout, this, [&ticks] { ++ticks; });
        ticker.start(10);
        QElapsedTimer clock;
        clock.start();
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 5000);
        QVERIFY(clock.elapsed() < 3000);
        QVERIFY(ticks >= 10); // the event loop kept running while waiting for the child
        QCOMPARE(finished.at(0).at(0).toInt(), 128 + 1); // SIGHUP
        QVERIFY(!session.isRunning());
    }

    void mainWindowClosesWhenShellExits() {
        MainWindow::Options options;
        options.session = shellOptions(QStringLiteral("exit 0"));
        MainWindow window(options);
        QVERIFY(window.start());
        window.show();
        QTRY_VERIFY_WITH_TIMEOUT(!window.isVisible(), 5000);
    }

    void mainWindowHoldsOnExitWhenRequested() {
        MainWindow::Options options;
        options.session = shellOptions(QStringLiteral("echo bye; exit 2"));
        options.holdOnExit = true;
        MainWindow window(options);
        QSignalSpy finished(window.session(), &TerminalSession::finished);
        QVERIFY(window.start());
        window.show();
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 5000);
        QVERIFY(window.isVisible());
        QVERIFY(screenContains(*window.session(), "bye"));
    }
};

QTEST_MAIN(AppTests)
#include "AppTests.moc"
