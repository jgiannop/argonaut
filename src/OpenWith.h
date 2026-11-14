#pragma once

#include <QList>
#include <QString>
#include <QStringList>

// "Open With" through GNOME's own tools (Linux): `gio mime` lists the apps
// for a file type, `gio launch` starts one exactly as the desktop would, and
// `gio mime TYPE APP` sets the default. ARGONAUT_GIO_PROGRAM replaces `gio`
// (tests use a stand-in that only logs).
namespace OpenWith {

struct App
{
    QString id;          // "org.gnome.TextEditor.desktop"
    QString name;        // "Text Editor" (translated if the file has it)
    QString icon;        // icon name or absolute path
    QString desktopFile; // full path
};

struct Apps
{
    QString defaultId;
    QStringList recommendedIds; // in gio's order
    QStringList registeredIds;
};

bool isSupported();
// Parse `LC_ALL=C gio mime TYPE` output.
Apps parseGioMime(const QString &output);
// Apps that can open `mimeType`: default first, then recommended, then the rest.
// Apps hidden from menus (NoDisplay/Hidden) are left out.
QList<App> appsFor(const QString &mimeType, QString *defaultId = nullptr);
App appFromDesktopFile(const QString &desktopFile);
bool launch(const App &app, const QStringList &files);
bool setDefault(const QString &mimeType, const App &app);

} // namespace OpenWith
