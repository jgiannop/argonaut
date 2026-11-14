#include "Trash.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QUrl>

#if defined(Q_OS_UNIX)
#include <unistd.h>
#endif

namespace {

QString uidString()
{
#if defined(Q_OS_UNIX)
    return QString::number(::getuid());
#else
    return {};
#endif
}

QString parentOf(const QString &path)
{
    return QFileInfo(path).absolutePath();
}

// The drive's top folder for a topdir trash ".../.Trash-uid/files" or
// ".../.Trash/uid/files" (relative paths in .trashinfo start there).
QString topDirOf(const QString &filesDir)
{
    const QString base = parentOf(filesDir); // .Trash-uid  or  .Trash/uid
    if (QFileInfo(base).fileName().startsWith(QLatin1String(".Trash-")))
        return parentOf(base);
    return parentOf(parentOf(base));
}

} // namespace

namespace Trash {

bool isSupported()
{
#if defined(Q_OS_LINUX) || defined(Q_OS_FREEBSD)
    return true;
#else
    return false;
#endif
}

QString homeFilesDir()
{
    return QDir::cleanPath(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)
                           + QStringLiteral("/Trash/files"));
}

QStringList filesDirs()
{
    if (!isSupported())
        return {};
    QStringList dirs{homeFilesDir()};
    // Tests set this so they can never touch real drives' trash folders.
    if (qEnvironmentVariableIsSet("KOSMOS_TRASH_HOME_ONLY"))
        return dirs;
    const QString uid = uidString();
    for (const QStorageInfo &volume : QStorageInfo::mountedVolumes()) {
        if (!volume.isValid() || !volume.isReady() || volume.isReadOnly())
            continue;
        const QString root = volume.rootPath();
        for (const QString &candidate :
             {root + QStringLiteral("/.Trash-") + uid + QStringLiteral("/files"),
              root + QStringLiteral("/.Trash/") + uid + QStringLiteral("/files")}) {
            const QString dir = QDir::cleanPath(candidate);
            if (!dirs.contains(dir) && QFileInfo(dir).isDir())
                dirs.append(dir);
        }
    }
    return dirs;
}

bool isFilesDir(const QString &dir)
{
    if (!isSupported())
        return false;
    const QString clean = QDir::cleanPath(dir);
    if (clean == homeFilesDir())
        return true;
    if (QFileInfo(clean).fileName() != QLatin1String("files"))
        return false;
    const QString base = parentOf(clean);
    const QString baseName = QFileInfo(base).fileName();
    if (baseName == QLatin1String(".Trash-") + uidString())
        return true;
    return baseName == uidString() && QFileInfo(parentOf(base)).fileName() == QLatin1String(".Trash");
}

QString filesDirOf(const QString &path)
{
    QString dir = QDir::cleanPath(path);
    while (true) {
        if (isFilesDir(dir))
            return dir;
        const QString up = parentOf(dir);
        if (up == dir || up.isEmpty())
            return {};
        dir = up;
    }
}

QString infoPathFor(const QString &pathInTrash)
{
    const QString clean = QDir::cleanPath(pathInTrash);
    const QString filesDir = parentOf(clean);
    if (!isFilesDir(filesDir))
        return {}; // not a top-level trashed item
    return parentOf(filesDir) + QStringLiteral("/info/") + QFileInfo(clean).fileName()
           + QStringLiteral(".trashinfo");
}

Info info(const QString &pathInTrash)
{
    Info result;
    const QString infoPath = infoPathFor(pathInTrash);
    if (infoPath.isEmpty())
        return result;
    QFile file(infoPath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return result;

    bool inSection = false;
    while (!file.atEnd()) {
        const QByteArray line = file.readLine().trimmed();
        if (line.startsWith('[')) {
            inSection = line == "[Trash Info]";
            continue;
        }
        if (!inSection)
            continue;
        if (line.startsWith("Path=")) {
            QString path = QUrl::fromPercentEncoding(line.mid(5));
            if (!path.startsWith(QLatin1Char('/'))) // topdir trashes may store relative paths
                path = topDirOf(QDir::cleanPath(parentOf(QDir::cleanPath(pathInTrash))))
                       + QLatin1Char('/') + path;
            result.originalPath = QDir::cleanPath(path);
        } else if (line.startsWith("DeletionDate=")) {
            result.deletionDate =
                QDateTime::fromString(QString::fromLatin1(line.mid(13)), Qt::ISODate);
        }
    }
    result.valid = !result.originalPath.isEmpty();
    return result;
}

} // namespace Trash
