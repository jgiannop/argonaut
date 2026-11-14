#include "Bookmarks.h"

#include "Remote.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUrl>

namespace {

struct Line
{
    QByteArray raw;   // the line as it was (written back untouched if not ours)
    QString path;     // set for local folders
    QString label;
};

QList<Line> readLines()
{
    QList<Line> lines;
    QFile file(Bookmarks::filePath());
    if (!file.open(QIODevice::ReadOnly))
        return lines;
    for (const QByteArray &raw : file.readAll().split('\n')) {
        if (raw.trimmed().isEmpty())
            continue;
        Line line{raw, {}, {}};
        const int space = int(raw.indexOf(' '));
        const QUrl url = QUrl::fromEncoded(space < 0 ? raw : raw.left(space));
        if (url.isLocalFile()) {
            line.path = QDir::cleanPath(url.toLocalFile());
            line.label = space < 0 ? QString() : QString::fromUtf8(raw.mid(space + 1)).trimmed();
        }
        lines.append(line);
    }
    return lines;
}

} // namespace

QList<Bookmarks::Bookmark> Bookmarks::loadRemote()
{
    QList<Bookmark> out;
    QFile file(filePath());
    if (!file.open(QIODevice::ReadOnly))
        return out;
    for (const QByteArray &raw : file.readAll().split('\n')) {
        const QByteArray line = raw.trimmed();
        const int space = int(line.indexOf(' '));
        const QString address = QString::fromUtf8(space < 0 ? line : line.left(space));
        if (!Remote::isRemoteAddress(address))
            continue;
        out.append({address, space < 0 ? QString() : QString::fromUtf8(line.mid(space + 1)).trimmed()});
    }
    return out;
}

namespace {

QByteArray encode(const QString &path, const QString &label)
{
    QByteArray line = QUrl::fromLocalFile(path).toEncoded();
    if (!label.isEmpty())
        line += ' ' + label.toUtf8();
    return line;
}

bool writeLines(const QList<Line> &lines)
{
    const QString path = Bookmarks::filePath();
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile file(path); // atomic: Nautilus never sees a half-written file
    if (!file.open(QIODevice::WriteOnly))
        return false;
    for (const Line &line : lines)
        file.write((line.path.isEmpty() ? line.raw : encode(line.path, line.label)) + '\n');
    return file.commit();
}

} // namespace

namespace Bookmarks {

QString Bookmark::displayName() const
{
    if (!label.isEmpty())
        return label;
    const QString name = QFileInfo(path).fileName();
    return name.isEmpty() ? QDir::toNativeSeparators(path) : name;
}

QString filePath()
{
#if defined(Q_OS_LINUX) || defined(Q_OS_FREEBSD)
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)
           + QStringLiteral("/gtk-3.0/bookmarks");
#else
    return QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation)
           + QStringLiteral("/bookmarks");
#endif
}

QList<Bookmark> load()
{
    QList<Bookmark> bookmarks;
    for (const Line &line : readLines()) {
        if (!line.path.isEmpty())
            bookmarks.append({line.path, line.label});
    }
    return bookmarks;
}

bool contains(const QString &path)
{
    const QString clean = QDir::cleanPath(path);
    for (const Bookmark &b : load()) {
        if (b.path == clean)
            return true;
    }
    return false;
}

bool add(const QString &path)
{
    if (contains(path))
        return true;
    QList<Line> lines = readLines();
    lines.append({QByteArray(), QDir::cleanPath(path), QString()});
    return writeLines(lines);
}

bool remove(const QString &path)
{
    const QString clean = QDir::cleanPath(path);
    QList<Line> lines = readLines();
    const auto removed = lines.removeIf([&](const Line &l) { return l.path == clean; });
    return removed > 0 && writeLines(lines);
}

bool rename(const QString &path, const QString &label)
{
    const QString clean = QDir::cleanPath(path);
    QList<Line> lines = readLines();
    bool found = false;
    for (Line &line : lines) {
        if (line.path == clean) {
            line.label = label.trimmed();
            found = true;
        }
    }
    return found && writeLines(lines);
}

} // namespace Bookmarks
