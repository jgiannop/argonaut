#include "DesktopActions.h"

#include "FileJob.h"
#include "Tools.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QUrl>

namespace {

QString tr(const char *text)
{
    return QCoreApplication::translate("DesktopActions", text);
}

constexpr int MaxScriptDepth = 3;

QList<DesktopActions::Script> scriptsIn(const QString &folder, int depth)
{
    QList<DesktopActions::Script> found;
    const QFileInfoList entries =
        QDir(folder).entryInfoList(QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot, QDir::Name | QDir::DirsFirst);
    for (const QFileInfo &entry : entries) {
        if (entry.isDir() && depth < MaxScriptDepth) {
            const QList<DesktopActions::Script> inside = scriptsIn(entry.filePath(), depth + 1);
            if (!inside.isEmpty())
                found.append({entry.fileName(), QString(), inside});
        } else if (entry.isFile() && entry.isExecutable()) {
            found.append({entry.fileName(), entry.filePath(), {}});
        }
    }
    return found;
}

// "a\nb\n" as Nautilus passes lists; nothing for an empty list.
QString lines(const QStringList &items)
{
    return items.isEmpty() ? QString() : items.join(QLatin1Char('\n')) + QLatin1Char('\n');
}

} // namespace

namespace DesktopActions {

bool sendByEmail(const QStringList &paths)
{
    QStringList args;
    for (const QString &path : paths)
        args << QStringLiteral("--attach") << path;
    return QProcess::startDetached(Tools::program("ARGONAUT_EMAIL_PROGRAM", QStringLiteral("xdg-email")), args);
}

bool setAsBackground(const QString &image, QString *error)
{
    const QString dir =
        QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + QStringLiteral("/backgrounds");
    QDir().mkpath(dir);
    const QFileInfo original(image);
    QString copy = dir + QLatin1Char('/') + original.fileName();
    // Another picture already has this name there: keep both.
    if (QFileInfo(copy).exists() && QFileInfo(copy).size() != original.size())
        copy = uniqueTargetPath(dir, original.fileName(), false);
    if (!QFileInfo::exists(copy) && !QFile::copy(image, copy)) {
        if (error)
            *error = tr("Couldn't copy the image for the background");
        return false;
    }
    const QString uri = QUrl::fromLocalFile(copy).toString();
    const QString gsettings = Tools::program("ARGONAUT_GSETTINGS_PROGRAM", QStringLiteral("gsettings"));
    bool ok = true;
    for (const char *key : {"picture-uri", "picture-uri-dark"}) {
        const int exitCode = QProcess::execute(gsettings, {QStringLiteral("set"), QStringLiteral("org.gnome.desktop.background"),
                                                           QString::fromLatin1(key), uri});
        ok = exitCode == 0 && ok;
    }
    if (!ok && error)
        *error = tr("Couldn't change the background (GNOME only)");
    return ok;
}

QString scriptsFolder()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + QStringLiteral("/nautilus/scripts");
}

QList<Script> scripts(const QString &folder)
{
    return scriptsIn(folder, 0);
}

bool runScript(const QString &script, const QString &currentFolder, const QStringList &selection,
               const QRect &windowGeometry)
{
    QStringList uris, args;
    for (const QString &path : selection) {
        uris << QUrl::fromLocalFile(path).toString();
        args << (QFileInfo(path).absolutePath() == currentFolder ? QFileInfo(path).fileName() : path);
    }
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("NAUTILUS_SCRIPT_SELECTED_FILE_PATHS"), lines(selection));
    env.insert(QStringLiteral("NAUTILUS_SCRIPT_SELECTED_URIS"), lines(uris));
    env.insert(QStringLiteral("NAUTILUS_SCRIPT_CURRENT_URI"), QUrl::fromLocalFile(currentFolder).toString());
    env.insert(QStringLiteral("NAUTILUS_SCRIPT_WINDOW_GEOMETRY"),
               QStringLiteral("%1x%2+%3+%4")
                   .arg(windowGeometry.width())
                   .arg(windowGeometry.height())
                   .arg(windowGeometry.x())
                   .arg(windowGeometry.y()));
    QProcess process;
    process.setProcessEnvironment(env);
    process.setProgram(script);
    process.setArguments(args);
    process.setWorkingDirectory(currentFolder);
    return process.startDetached();
}

} // namespace DesktopActions
