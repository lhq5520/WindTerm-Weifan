#pragma once

#include <QString>
#include <QStringList>

class QCoreApplication;

// User interface languages. The application's own messages are compiled into the binary
// (:/i18n/windterm-weifan_<locale>.qm); Qt's standard dialogs use Qt's translations when the
// deployment has them.
namespace Translations {

// (code, native name) of the shipped translations, English first.
QList<QPair<QString, QString>> available();
// Installs the translators for `language` ("" = the system language, "en" = no translation).
// Returns the code that is in effect ("en" when nothing matched).
QString install(QCoreApplication* app, const QString& language);
// Removes what install() added (tests switch languages).
void uninstall(QCoreApplication* app);
// Translated title of a key-bindable action (wt::session::ActionInfo::title).
QString actionTitle(const char* englishTitle);

} // namespace Translations
