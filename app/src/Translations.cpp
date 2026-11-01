#include "Translations.h"

#include <QCoreApplication>
#include <QLibraryInfo>
#include <QLocale>
#include <QPointer>
#include <QTranslator>

namespace {

QList<QPointer<QTranslator>>& installed() {
    static QList<QPointer<QTranslator>> translators;
    return translators;
}

// Titles of the key-bindable actions (wt::session::allActions(), which is Qt-free), listed here
// so lupdate extracts them. Keep in sync with libs/session/src/Keymap.cpp.
[[maybe_unused]] const char* const kActionTitles[] = {
    QT_TRANSLATE_NOOP("Action", "New Tab"),
    QT_TRANSLATE_NOOP("Action", "Close Tab"),
    QT_TRANSLATE_NOOP("Action", "Next Tab"),
    QT_TRANSLATE_NOOP("Action", "Previous Tab"),
    QT_TRANSLATE_NOOP("Action", "Split Right"),
    QT_TRANSLATE_NOOP("Action", "Split Down"),
    QT_TRANSLATE_NOOP("Action", "Close Pane"),
    QT_TRANSLATE_NOOP("Action", "Next Pane"),
    QT_TRANSLATE_NOOP("Action", "Previous Pane"),
    QT_TRANSLATE_NOOP("Action", "Find..."),
    QT_TRANSLATE_NOOP("Action", "Find Next"),
    QT_TRANSLATE_NOOP("Action", "Find Previous"),
    QT_TRANSLATE_NOOP("Action", "Copy"),
    QT_TRANSLATE_NOOP("Action", "Paste"),
    QT_TRANSLATE_NOOP("Action", "Select All"),
    QT_TRANSLATE_NOOP("Action", "Clear Scrollback"),
    QT_TRANSLATE_NOOP("Action", "Zoom In"),
    QT_TRANSLATE_NOOP("Action", "Zoom Out"),
    QT_TRANSLATE_NOOP("Action", "Actual Size"),
    QT_TRANSLATE_NOOP("Action", "New SSH Session..."),
    QT_TRANSLATE_NOOP("Action", "Session Manager..."),
    QT_TRANSLATE_NOOP("Action", "Settings..."),
    QT_TRANSLATE_NOOP("Action", "File Browser (SFTP)"),
    QT_TRANSLATE_NOOP("Action", "Reconnect"),
    QT_TRANSLATE_NOOP("Action", "New Telnet Session..."),
    QT_TRANSLATE_NOOP("Action", "New Serial Session..."),
    QT_TRANSLATE_NOOP("Action", "Send Break"),
    QT_TRANSLATE_NOOP("Action", "Port Forwarding..."),
    QT_TRANSLATE_NOOP("Action", "Log Session to File"),
    QT_TRANSLATE_NOOP("Action", "Broadcast Input to All Panes of the Tab"),
    QT_TRANSLATE_NOOP("Action", "Broadcast Input to All Tabs"),
};

} // namespace

namespace Translations {

QList<QPair<QString, QString>> available() {
    return {{QStringLiteral("en"), QStringLiteral("English")},
            {QStringLiteral("zh_CN"), QStringLiteral("简体中文")}};
}

void uninstall(QCoreApplication* app) {
    for (const QPointer<QTranslator>& translator : installed()) {
        if (translator) {
            app->removeTranslator(translator);
            delete translator;
        }
    }
    installed().clear();
}

QString install(QCoreApplication* app, const QString& language) {
    uninstall(app);
    const QLocale locale = language.isEmpty() ? QLocale::system() : QLocale(language);
    if (language == QLatin1String("en") || locale.language() == QLocale::English || locale.language() == QLocale::C) {
        return QStringLiteral("en");
    }
    auto* ours = new QTranslator(app);
    if (!ours->load(locale, QStringLiteral("windterm-weifan"), QStringLiteral("_"), QStringLiteral(":/i18n"))) {
        delete ours;
        return QStringLiteral("en");
    }
    app->installTranslator(ours);
    installed() << ours;
    // Qt's own strings (standard buttons, file dialogs), when the Qt installation ships them.
    auto* qt = new QTranslator(app);
    if (qt->load(locale, QStringLiteral("qtbase"), QStringLiteral("_"), QLibraryInfo::path(QLibraryInfo::TranslationsPath))) {
        app->installTranslator(qt);
        installed() << qt;
    } else {
        delete qt;
    }
    return ours->language();
}

QString actionTitle(const char* englishTitle) {
    return QCoreApplication::translate("Action", englishTitle);
}

} // namespace Translations
