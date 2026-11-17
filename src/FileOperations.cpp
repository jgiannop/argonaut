#include "FileOperations.h"

#include <algorithm>

#include "Trash.h"

#include <QApplication>
#include <QClipboard>
#include <QDir>
#include <QFileInfo>
#include <QMimeData>
#include <QUrl>

namespace {

constexpr int MaxUndo = 50;
const QString GnomeFormat = QStringLiteral("x-special/gnome-copied-files");
const QString KdeCutFormat = QStringLiteral("application/x-kde-cutselection");
#if defined(Q_OS_WIN)
// Explorer's "Preferred DropEffect" (DROPEFFECT_COPY = 1, DROPEFFECT_MOVE = 2).
const QString WinDropEffect =
    QStringLiteral("application/x-qt-windows-mime;value=\"Preferred DropEffect\"");
#endif

QList<Transfer> transfersInto(const QStringList &paths, const QString &targetDir)
{
    QList<Transfer> transfers;
    for (const QString &path : paths)
        transfers.append({path, targetDir, QFileInfo(path).fileName()});
    return transfers;
}

} // namespace

FileOperations::FileOperations(QObject *parent)
    : QObject(parent)
{
    m_pool.setMaxThreadCount(2);
}

FileOperations::~FileOperations()
{
    cancelAll();
    m_pool.waitForDone(); // jobs are our children; they must stop first
}

FileJob *FileOperations::copy(const QStringList &paths, const QString &targetDir)
{
    return startJob(FileJob::Kind::Copy, transfersInto(paths, targetDir), true, true);
}

FileJob *FileOperations::move(const QStringList &paths, const QString &targetDir)
{
    return startJob(FileJob::Kind::Move, transfersInto(paths, targetDir), true, true);
}

FileJob *FileOperations::moveItems(const QList<Transfer> &transfers, const QString &action)
{
    return startJob(FileJob::Kind::Move, transfers, true, true, action);
}

FileJob *FileOperations::copyItems(const QList<Transfer> &transfers, const QString &action)
{
    return startJob(FileJob::Kind::Copy, transfers, true, true, action);
}

FileJob *FileOperations::trash(const QStringList &paths)
{
    return startJob(FileJob::Kind::Trash, transfersInto(paths, QString()), true, true);
}

FileJob *FileOperations::extract(const QStringList &archives, const QString &targetDir)
{
    return startJob(FileJob::Kind::Extract, transfersInto(archives, targetDir), true, true);
}

FileJob *FileOperations::compress(const QStringList &paths, const QString &targetDir,
                                  const QString &archiveName, FileJob::ArchiveFormat format)
{
    QList<Transfer> transfers;
    for (const QString &path : paths)
        transfers.append({path, targetDir, archiveName});
    // startJob() starts the job at once, so it applies the format from here.
    if (transfers.isEmpty())
        return nullptr;
    m_pendingFormat = format;
    return startJob(FileJob::Kind::Compress, transfers, true, true);
}

bool FileOperations::isArchive(const QString &mimeType)
{
    static const QStringList archives = {
        "application/zip", "application/x-tar", "application/x-compressed-tar",
        "application/x-bzip-compressed-tar", "application/x-bzip2-compressed-tar",
        "application/x-xz-compressed-tar", "application/x-zstd-compressed-tar",
        "application/x-lzma-compressed-tar", "application/x-7z-compressed", "application/vnd.rar",
        "application/x-rar", "application/x-rar-compressed", "application/gzip", "application/x-xz",
        "application/x-bzip", "application/x-bzip2", "application/zstd", "application/x-cpio",
        "application/x-iso9660-image", "application/x-cd-image", "application/vnd.debian.binary-package",
        "application/x-rpm", "application/java-archive", "application/x-lha", "application/x-archive",
    };
    return FileJob::archivesSupported() && archives.contains(mimeType);
}

FileJob *FileOperations::deletePermanently(const QStringList &paths)
{
    return startJob(FileJob::Kind::Delete, transfersInto(paths, QString()), false, true);
}

FileJob *FileOperations::restore(const QStringList &pathsInTrash)
{
    QList<Transfer> transfers;
    for (const QString &path : pathsInTrash) {
        const Trash::Info info = Trash::info(path);
        if (info.valid) {
            const QFileInfo original(info.originalPath);
            transfers.append({path, original.absolutePath(), original.fileName()});
        } else {
            // No record of where it came from: put it in the home folder.
            transfers.append({path, QDir::homePath(), QFileInfo(path).fileName()});
        }
    }
    return startJob(FileJob::Kind::Restore, transfers, false, true);
}

FileJob *FileOperations::emptyTrash()
{
    QStringList items;
    for (const QString &filesDir : Trash::filesDirs()) {
        const QDir files(filesDir);
        for (const QFileInfo &entry : files.entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot
                                                          | QDir::Hidden | QDir::System))
            items.append(entry.absoluteFilePath());
        // Stray .trashinfo files whose item is already gone.
        const QDir info(QFileInfo(filesDir).absolutePath() + QStringLiteral("/info"));
        for (const QFileInfo &entry : info.entryInfoList({QStringLiteral("*.trashinfo")},
                                                         QDir::Files | QDir::Hidden)) {
            const QString item = filesDir + QLatin1Char('/') + entry.completeBaseName();
            if (!QFileInfo::exists(item))
                items.append(entry.absoluteFilePath());
        }
    }
    return startJob(FileJob::Kind::Delete, transfersInto(items, QString()), false, true);
}

void FileOperations::cancelAll()
{
    for (FileJob *job : std::as_const(m_active))
        job->cancel();
}

FileJob *FileOperations::startJob(FileJob::Kind kind, QList<Transfer> transfers, bool recordUndo,
                                  bool userAction, const QString &undoName, const std::function<void(FileJob *)> &prepare)
{
    if (transfers.isEmpty())
        return nullptr;
    auto *job = new FileJob(kind, std::move(transfers), this);
    job->setProperty(UserActionProperty, userAction);
    job->setInteractive(m_askOnConflict);
    job->setArchiveFormat(m_pendingFormat);
    connect(job, &FileJob::conflict, this, [this, job](const QString &source, const QString &existing) {
        emit conflictRaised(job, source, existing);
    });
    m_active.append(job);
    if (prepare)
        prepare(job); // before ours below: the history is up to date when jobFinished is heard
    connect(job, &FileJob::finished, this, [this, job, recordUndo, undoName] {
        m_active.removeOne(job);
        const QList<TransferResult> results = job->results();
        if (recordUndo && !results.isEmpty()) {
            pushUndo({job->kind(), results, job->kind(), undoName});
            m_redo.clear(); // a new action: the old future is gone
            emit undoChanged();
        }
        emit jobFinished(job);
        job->deleteLater();
    });
    emit jobStarted(job);
    job->start(&m_pool);
    return job;
}

void FileOperations::pushUndo(const UndoEntry &entry)
{
    m_undo.append(entry);
    while (m_undo.size() > MaxUndo)
        m_undo.removeFirst();
}

void FileOperations::recordUndo(FileJob::Kind kind, const QList<TransferResult> &results,
                                const QString &action)
{
    if (results.isEmpty())
        return;
    pushUndo({kind, results, kind, action});
    m_redo.clear(); // a new action: the old future is gone
    emit undoChanged();
}

QString FileOperations::historyText(const QList<UndoEntry> &list, bool undo) const
{
    if (list.isEmpty())
        return undo ? tr("Undo") : tr("Redo");
    const UndoEntry &e = list.last();
    if (!e.name.isEmpty())
        return (undo ? tr("Undo %1") : tr("Redo %1")).arg(e.name);
    int n = 0;
    for (const TransferResult &r : e.results)
        n += r.target.isEmpty() ? 0 : 1;
    const QString items = n == 1 ? tr("1 item") : tr("%1 items").arg(n);
    switch (e.action) {
    case FileJob::Kind::Copy:
        return (undo ? tr("Undo Copy (%1)") : tr("Redo Copy (%1)")).arg(items);
    case FileJob::Kind::Move:
        return (undo ? tr("Undo Move (%1)") : tr("Redo Move (%1)")).arg(items);
    case FileJob::Kind::Trash:
        return (undo ? tr("Undo Move to Trash (%1)") : tr("Redo Move to Trash (%1)")).arg(items);
    case FileJob::Kind::Extract:
        return undo ? tr("Undo Extract") : tr("Redo Extract");
    case FileJob::Kind::Compress:
        return undo ? tr("Undo Compress") : tr("Redo Compress");
    default:
        break;
    }
    return undo ? tr("Undo") : tr("Redo");
}

QString FileOperations::undoText() const
{
    return historyText(m_undo, true);
}

QString FileOperations::redoText() const
{
    return historyText(m_redo, false);
}

// Run the inverse of the last operation. Undoing a copy moves the copies to
// the trash rather than deleting them, so nothing is ever lost.
void FileOperations::undo()
{
    if (m_undo.isEmpty())
        return;
    const UndoEntry entry = m_undo.takeLast();
    runInverse(entry, &m_redo, "undo");
    emit undoChanged();
}

void FileOperations::redo()
{
    if (m_redo.isEmpty())
        return;
    const UndoEntry entry = m_redo.takeLast();
    runInverse(entry, &m_undo, "redo");
    emit undoChanged();
}

void FileOperations::runInverse(const UndoEntry &entry, QList<UndoEntry> *onto, const char *history)
{
    QList<Transfer> back;     // each result moved back to where it came from
    QStringList created;      // results to put in the Trash
    for (const TransferResult &r : entry.results) {
        if (r.target.isEmpty())
            continue;
        const QFileInfo original(r.source);
        back.append({r.target, original.absolutePath(), original.fileName()});
        created.append(r.target);
    }

    // Set up before the job starts: a quick job (one rename) can be done
    // before we get any further, and its inverse must still be recorded,
    // or there would be nothing to redo.
    const auto prepare = [this, onto, history, action = entry.action, name = entry.name](FileJob *job) {
        job->setProperty(HistoryProperty, QString::fromLatin1(history));
        // When it's done, what it did can be reversed in turn.
        connect(job, &FileJob::finished, this, [this, job, onto, action, name] {
            const QList<TransferResult> results = job->results();
            if (results.isEmpty())
                return;
            const UndoEntry next{job->kind(), results, action, name};
            if (onto == &m_undo)
                pushUndo(next);
            else
                onto->append(next);
            emit undoChanged();
        });
    };
    switch (entry.kind) {
    case FileJob::Kind::Copy:     // copies, extracted files, new items…
    case FileJob::Kind::Extract:
    case FileJob::Kind::Compress:
    case FileJob::Kind::Restore:  // …and what came back from the Trash
        startJob(FileJob::Kind::Trash, transfersInto(created, QString()), false, false, QString(), prepare);
        break;
    case FileJob::Kind::Move:
        // Backwards: steps that depend on each other (a batch rename's
        // "b -> c" before "a -> b") must be undone in reverse order.
        std::reverse(back.begin(), back.end());
        startJob(FileJob::Kind::Move, back, false, false, QString(), prepare);
        break;
    case FileJob::Kind::Trash:
        startJob(FileJob::Kind::Restore, back, false, false, QString(), prepare);
        break;
    case FileJob::Kind::Delete: // permanent: nothing to reverse
        break;
    }
}

// ------------------------------------------------------------ clipboard ---

void FileOperations::setClipboardFiles(const QStringList &paths, bool cut)
{
    auto *mime = new QMimeData;
    QList<QUrl> urls;
    QByteArray gnome = cut ? "cut" : "copy";
    QStringList native;
    for (const QString &path : paths) {
        const QUrl url = QUrl::fromLocalFile(path);
        urls.append(url);
        gnome += '\n' + url.toEncoded();
        native.append(QDir::toNativeSeparators(path));
    }
    mime->setUrls(urls);
    mime->setData(GnomeFormat, gnome);
    if (cut)
        mime->setData(KdeCutFormat, "1");
#if defined(Q_OS_WIN)
    QByteArray effect(4, '\0');
    effect[0] = cut ? 2 : 1;
    mime->setData(WinDropEffect, effect);
#endif
    mime->setText(native.join('\n'));
    QApplication::clipboard()->setMimeData(mime);
}

bool FileOperations::clipboardFiles(QStringList *paths, bool *cut)
{
    paths->clear();
    *cut = false;
    const QMimeData *mime = QApplication::clipboard()->mimeData();
    if (!mime)
        return false;

    QList<QUrl> urls;
    if (mime->hasFormat(GnomeFormat)) {
        const QList<QByteArray> lines = mime->data(GnomeFormat).split('\n');
        if (!lines.isEmpty())
            *cut = lines.first().trimmed() == "cut";
        for (int i = 1; i < lines.size(); ++i) {
            const QByteArray line = lines.at(i).trimmed();
            if (!line.isEmpty())
                urls.append(QUrl::fromEncoded(line));
        }
    } else if (mime->hasUrls()) {
        urls = mime->urls();
        *cut = mime->data(KdeCutFormat).startsWith('1');
#if defined(Q_OS_WIN)
        const QByteArray effect = mime->data(WinDropEffect);
        if (!effect.isEmpty())
            *cut = (effect.at(0) & 2) != 0;
#endif
    }

    for (const QUrl &url : std::as_const(urls)) {
        if (url.isLocalFile())
            paths->append(QDir::cleanPath(url.toLocalFile()));
    }
    return !paths->isEmpty();
}

bool FileOperations::clipboardHasFiles()
{
    const QMimeData *mime = QApplication::clipboard()->mimeData();
    return mime && (mime->hasFormat(GnomeFormat) || mime->hasUrls());
}
