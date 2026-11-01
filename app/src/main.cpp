#include "AppContext.h"
#include "MainWindow.h"
#include "Translations.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QIcon>
#include <QMessageBox>

#include <algorithm>
#include <cstdio>

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("WindTerm-Weifan"));
    QApplication::setApplicationDisplayName(QStringLiteral("WindTerm-Weifan"));
    QApplication::setApplicationVersion(QStringLiteral(WT_VERSION));
    // No organization name: settings live in <config>/WindTerm-Weifan, not .../WindTerm-Weifan/WindTerm-Weifan.
    QApplication::setDesktopFileName(QStringLiteral("windterm-weifan"));
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/icons/app.png")));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Cross-platform terminal emulator"));
    parser.addHelpOption();
    parser.addVersionOption();
    const QCommandLineOption cwdOption(QStringLiteral("cwd"), QStringLiteral("Start in <dir>."),
                                       QStringLiteral("dir"));
    const QCommandLineOption holdOption(QStringLiteral("hold"),
                                        QStringLiteral("Keep the window open after the program exits."));
    const QCommandLineOption loginOption(QStringLiteral("login"), QStringLiteral("Run the shell as a login shell."));
    const QCommandLineOption sizeOption(QStringLiteral("geometry"), QStringLiteral("Initial size, e.g. 120x40."),
                                        QStringLiteral("COLSxROWS"), QStringLiteral("80x24"));
    const QCommandLineOption profileOption(QStringLiteral("profile"), QStringLiteral("Open the saved session <id>."),
                                           QStringLiteral("id"));
    const QCommandLineOption configOption(QStringLiteral("config-dir"),
                                          QStringLiteral("Read settings and sessions from <dir>."), QStringLiteral("dir"));
    const QCommandLineOption noConfigOption(QStringLiteral("no-config"),
                                            QStringLiteral("Use default settings and do not save anything."));
    parser.addOptions({cwdOption, holdOption, loginOption, sizeOption, profileOption, configOption, noConfigOption});
    parser.addPositionalArgument(QStringLiteral("command"),
                                 QStringLiteral("Program to run instead of the login shell (use -- before "
                                                "options meant for the program)."),
                                 QStringLiteral("[-- command [args...]]"));
    parser.process(app);

    MainWindow::Options options;
    const QStringList positional = parser.positionalArguments();
    if (!positional.isEmpty()) {
        options.session.program = positional.first();
        options.session.arguments = positional.mid(1);
    } else {
#ifdef Q_OS_MACOS
        // Like Terminal.app/iTerm2: login shells pick up the user's PATH when launched from Finder.
        options.session.loginShell = true;
#endif
    }
    if (parser.isSet(loginOption)) {
        options.session.loginShell = true;
    }
    options.session.workingDirectory = parser.isSet(cwdOption) ? parser.value(cwdOption) : QDir::homePath();
    options.holdOnExit = parser.isSet(holdOption);

    const QStringList geometry = parser.value(sizeOption).split(QLatin1Char('x'));
    if (geometry.size() == 2) {
        options.session.columns = std::clamp(geometry[0].toInt(), 10, 1000);
        options.session.rows = std::clamp(geometry[1].toInt(), 2, 500);
    }

    options.profileId = parser.value(profileOption);

    AppContext* context = parser.isSet(noConfigOption) ? AppContext::createEphemeral(&app)
                                                       : new AppContext(parser.value(configOption), &app);
    for (const QString& warning : context->warnings()) {
        std::fprintf(stderr, "warning: %s\n", qPrintable(warning));
    }
    // WT_LANGUAGE overrides the setting (handy for screenshots and tests of translations).
    const QString languageOverride = qEnvironmentVariable("WT_LANGUAGE");
    Translations::install(&app, languageOverride.isEmpty() ? QString::fromStdString(context->settings().language)
                                                           : languageOverride);

    MainWindow window(options, context);
    // Without an explicit command or session, reopen what was open when the app last quit.
    bool restored = false;
    const bool explicitStart = !positional.isEmpty() || !options.profileId.isEmpty() || parser.isSet(cwdOption);
    if (!explicitStart && context->isPersistent() && context->settings().restoreWorkspace) {
        if (const auto workspace = context->loadWorkspace(); workspace && !workspace->windows.empty()) {
            restored = window.restoreState(workspace->windows.front());
        }
    }
    if (!restored && !window.start()) {
        const QString message = QObject::tr("Failed to start the terminal session:\n%1").arg(window.errorString());
        std::fprintf(stderr, "%s\n", qPrintable(message));
        const QString platform = QGuiApplication::platformName();
        if (platform != QLatin1String("offscreen") && platform != QLatin1String("minimal")) {
            QMessageBox::critical(nullptr, QApplication::applicationDisplayName(), message);
        }
        return 1;
    }
    window.show();
    return app.exec();
}
