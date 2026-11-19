#include "TextFile.h"

#include "ContentType.h"
#include "Trash.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QStringDecoder>
#include <QTemporaryFile>

namespace TextFile {

namespace {

const QByteArray Bom("\xEF\xBB\xBF");

void stamp(Loaded *file)
{
    const QFileInfo info(file->path);
    file->size = info.exists() ? info.size() : -1;
    file->mtimeMs = info.exists() ? info.lastModified().toMSecsSinceEpoch() : -1;
}

// Why these bytes aren't text to edit; empty: they are.
QString dataProblem(const QByteArray &data)
{
    if (data.contains('\0'))
        return QStringLiteral("isn't text");
    QStringDecoder decoder(QStringDecoder::Utf8);
    const QString text = decoder.decode(data);
    if (decoder.hasError())
        return QStringLiteral("isn't UTF-8 text");
    return {};
}

} // namespace

QString problem(const QString &path)
{
    const QFileInfo info(path);
    if (!info.exists())
        return QStringLiteral("is gone");
    if (!info.isFile())
        return QStringLiteral("isn't a file");
    const QString real = info.canonicalFilePath();
    if (!Trash::filesDirOf(real).isEmpty())
        return QStringLiteral("is in the Trash (restore it first)");
    if (info.size() > MaxBytes)
        return QStringLiteral("is too big to edit here (over 5 MB)");
    if (!QFileInfo(real).isWritable())
        return QStringLiteral("is read-only");
    if (!QFileInfo(QFileInfo(real).absolutePath()).isWritable())
        return QStringLiteral("is in a folder you can't write to");
    if (ContentType::inspect(real).verdict == ContentType::Verdict::Dangerous)
        return QStringLiteral("is a program disguised as something else");
    QFile file(real);
    if (!file.open(QIODevice::ReadOnly))
        return QStringLiteral("can't be read");
    return dataProblem(file.readAll());
}

Loaded load(const QString &path)
{
    Loaded loaded;
    loaded.problem = problem(path);
    if (!loaded.problem.isEmpty())
        return loaded;
    loaded.path = QFileInfo(path).canonicalFilePath();
    QFile file(loaded.path);
    if (!file.open(QIODevice::ReadOnly)) {
        loaded.problem = QStringLiteral("can't be read");
        return loaded;
    }
    QByteArray data = file.readAll();
    loaded.permissions = file.permissions();
    if (data.startsWith(Bom)) {
        loaded.bom = true;
        data.remove(0, Bom.size());
    }
    loaded.problem = dataProblem(data);
    if (!loaded.problem.isEmpty())
        return loaded;
    loaded.text = QString::fromUtf8(data);
    if (loaded.text.contains(QStringLiteral("\r\n"))) {
        loaded.crlf = true;
        loaded.text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    }
    stamp(&loaded);
    return loaded;
}

bool changedOnDisk(const Loaded &file)
{
    const QFileInfo info(file.path);
    return !info.exists() || info.size() != file.size || info.lastModified().toMSecsSinceEpoch() != file.mtimeMs;
}

// A copy goes in the file's place for a moment: the file itself is moved to
// the Trash (so the Trash records the right place), and the copy, with the
// same bytes and permissions, takes its name back.
QString keepOldVersion(const QString &path, QString *error)
{
    const QFileInfo info(path);
    QTemporaryFile copy(info.absolutePath() + QStringLiteral("/.") + info.fileName() + QStringLiteral(".argonaut-old-XXXXXX"));
    copy.setAutoRemove(true);
    QFile original(path);
    if (!copy.open() || !original.open(QIODevice::ReadOnly)) {
        *error = QStringLiteral("couldn't make a copy of it");
        return {};
    }
    const QByteArray data = original.readAll();
    original.close();
    if (copy.write(data) != data.size() || !copy.flush()) {
        *error = QStringLiteral("couldn't make a copy of it");
        return {};
    }
    copy.setPermissions(original.permissions());
    copy.close();
    QString inTrash;
    if (!QFile::moveToTrash(path, &inTrash)) {
        *error = QStringLiteral("there is no Trash here to keep the old version in");
        return {};
    }
    if (!QFile::rename(copy.fileName(), path)) {
        // The file is in the Trash and the copy couldn't take its place:
        // bring it straight back, so it is never missing.
        QFile::rename(inTrash, path);
        QFile::remove(Trash::infoPathFor(inTrash));
        *error = QStringLiteral("couldn't put the copy in its place");
        return {};
    }
    copy.setAutoRemove(false); // it is the file now
    return inTrash.isEmpty() ? QStringLiteral("(in the Trash)") : inTrash;
}

void forgetOldVersion(const QString &pathInTrash)
{
    if (pathInTrash.isEmpty() || Trash::filesDirOf(pathInTrash).isEmpty())
        return; // only ever something in a Trash
    const QString info = Trash::infoPathFor(pathInTrash);
    if (QFile::remove(pathInTrash) && !info.isEmpty())
        QFile::remove(info);
}

Saved save(Loaded *file, const QString &text, bool keepOld)
{
    Saved saved;
    if (keepOld) {
        saved.oldVersion = keepOldVersion(file->path, &saved.error);
        if (saved.oldVersion.isEmpty())
            return saved;
    }
    QString written = text;
    if (file->crlf)
        written.replace(QLatin1Char('\n'), QStringLiteral("\r\n"));
    QByteArray data = written.toUtf8();
    if (file->bom)
        data.prepend(Bom);
    QSaveFile out(file->path);
    if (!out.open(QIODevice::WriteOnly) || out.write(data) != data.size() || !out.commit()) {
        saved.error = out.errorString();
        return saved;
    }
    QFile::setPermissions(file->path, file->permissions);
    stamp(file);
    saved.ok = true;
    return saved;
}

} // namespace TextFile
