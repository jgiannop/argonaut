#include "DefaultFileManager.h"

#include "OpenWith.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>

namespace {

constexpr const char *FolderMime = "inode/directory";
constexpr const char *NautilusId = "org.gnome.Nautilus.desktop";
constexpr const char *Marker = "# Written by Argonaut (argonaut --make-default).";

QString tr(const char *text)
{
    return QCoreApplication::translate("DefaultFileManager", text);
}

} // namespace

namespace DefaultFileManager {

QString desktopId()
{
    return QStringLiteral("argonaut.desktop");
}

QString installedDesktopFile()
{
    return QStandardPaths::locate(QStandardPaths::ApplicationsLocation, desktopId());
}

QString installedExecutable()
{
    const QString desktop = installedDesktopFile();
    if (desktop.isEmpty())
        return {};
    QFile file(desktop);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};
    bool inEntry = false;
    while (!file.atEnd()) {
        const QString line = QString::fromUtf8(file.readLine()).trimmed();
        if (line.startsWith(QLatin1Char('[')))
            inEntry = line == QLatin1String("[Desktop Entry]");
        else if (inEntry && line.startsWith(QLatin1String("Exec="))) {
            const QString program = QProcess::splitCommand(line.mid(5)).value(0);
            return QFileInfo(program).isAbsolute() ? program : QStandardPaths::findExecutable(program);
        }
    }
    return {};
}

QString currentDefault()
{
    QString id;
    OpenWith::appsFor(QString::fromLatin1(FolderMime), &id);
    return id;
}

bool isArgonautDefault()
{
    return currentDefault() == desktopId();
}

QString serviceFilePath()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)
           + QStringLiteral("/dbus-1/services/org.freedesktop.FileManager1.service");
}

bool serviceFileIsOurs()
{
    QFile file(serviceFilePath());
    if (!file.open(QIODevice::ReadOnly))
        return false;
    const QByteArray text = file.readAll();
    // Also ours: written under the app's earlier names.
    return text.contains(Marker) || text.contains("# Written by Kosmos") || text.contains("# Written by Kaftilous");
}

bool makeDefault(QString *error)
{
    auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return false;
    };
    const QString desktop = installedDesktopFile();
    const QString exe = installedExecutable();
    if (desktop.isEmpty() || exe.isEmpty() || !QFileInfo(exe).isExecutable())
        return fail(tr("Argonaut isn't installed yet (no argonaut.desktop). Install it first."));
    // A file we didn't write (another file manager's): leave it alone.
    if (QFile::exists(serviceFilePath()) && !serviceFileIsOurs())
        return fail(tr("Another program already handles “Show in folder” (%1). Remove that file first.")
                        .arg(serviceFilePath()));

    const QString previous = currentDefault();
    // Never remember ourselves under an old name as "the previous one".
    if (!previous.isEmpty() && previous != desktopId() && previous != QLatin1String("kosmos.desktop")
        && previous != QLatin1String("kaftilous.desktop"))
        QSettings().setValue("system/previousFileManager", previous);

    if (!OpenWith::setDefault(QString::fromLatin1(FolderMime), OpenWith::appFromDesktopFile(desktop)))
        return fail(tr("Couldn't set the default app for folders."));

    QDir().mkpath(QFileInfo(serviceFilePath()).absolutePath());
    QSaveFile service(serviceFilePath());
    if (!service.open(QIODevice::WriteOnly))
        return fail(tr("Couldn't write %1").arg(serviceFilePath()));
    service.write(QByteArray(Marker) + "\n# Undo: argonaut --restore-nautilus (or Preferences).\n"
                  "[D-BUS Service]\nName=org.freedesktop.FileManager1\nExec="
                  + QFile::encodeName(exe) + " --service\n");
    if (!service.commit())
        return fail(tr("Couldn't write %1").arg(serviceFilePath()));
    return true;
}

bool restorePrevious(QString *error, QString *restoredTo)
{
    auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return false;
    };
    // Our "Show in folder" hook goes first: Nautilus's system one takes over again.
    if (serviceFileIsOurs() && !QFile::remove(serviceFilePath()))
        return fail(tr("Couldn't remove %1").arg(serviceFilePath()));

    QString id = QSettings().value("system/previousFileManager").toString();
    QString file = id.isEmpty() ? QString() : QStandardPaths::locate(QStandardPaths::ApplicationsLocation, id);
    if (file.isEmpty()) { // gone (uninstalled) or never known: Nautilus
        id = QString::fromLatin1(NautilusId);
        file = QStandardPaths::locate(QStandardPaths::ApplicationsLocation, id);
    }
    if (file.isEmpty())
        return fail(tr("Couldn't find Nautilus (org.gnome.Nautilus.desktop) to give folders back to."));
    if (currentDefault() == desktopId() || currentDefault().isEmpty()) {
        if (!OpenWith::setDefault(QString::fromLatin1(FolderMime), OpenWith::appFromDesktopFile(file)))
            return fail(tr("Couldn't set the default app for folders."));
    }
    if (restoredTo)
        *restoredTo = id;
    return true;
}

} // namespace DefaultFileManager
