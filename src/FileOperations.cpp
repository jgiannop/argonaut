#include "FileOperations.h"

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
    return startJob(FileJob::Kind::Copy, transfersInto(paths, targetDir), true);
}

FileJob *FileOperations::move(const QStringList &paths, const QString &targetDir)
{
    return startJob(FileJob::Kind::Move, transfersInto(paths, targetDir), true);
}

FileJob *FileOperations::trash(const QStringList &paths)
{
    return startJob(FileJob::Kind::Trash, transfersInto(paths, QString()), true);
}

void FileOperations::cancelAll()
{
    for (FileJob *job : std::as_const(m_active))
        job->cancel();
}

FileJob *FileOperations::startJob(FileJob::Kind kind, QList<Transfer> transfers, bool recordUndo)
{
    if (transfers.isEmpty())
        return nullptr;
    auto *job = new FileJob(kind, std::move(transfers), this);
    job->setProperty(UserActionProperty, recordUndo);
    m_active.append(job);
    connect(job, &FileJob::finished, this, [this, job, recordUndo] {
        m_active.removeOne(job);
        const QList<TransferResult> results = job->results();
        if (recordUndo && !results.isEmpty()) {
            m_undo.append({job->kind(), results, QString()});
            while (m_undo.size() > MaxUndo)
                m_undo.removeFirst();
            emit undoChanged();
        }
        emit jobFinished(job);
        job->deleteLater();
    });
    emit jobStarted(job);
    job->start(&m_pool);
    return job;
}

void FileOperations::recordUndo(FileJob::Kind kind, const QList<TransferResult> &results,
                                const QString &label)
{
    if (results.isEmpty())
        return;
    m_undo.append({kind, results, label});
    while (m_undo.size() > MaxUndo)
        m_undo.removeFirst();
    emit undoChanged();
}

QString FileOperations::undoText() const
{
    if (m_undo.isEmpty())
        return tr("Undo");
    const UndoEntry &e = m_undo.last();
    if (!e.label.isEmpty())
        return e.label;
    const int n = int(e.results.size());
    const QString items = n == 1 ? tr("1 item") : tr("%1 items").arg(n);
    switch (e.kind) {
    case FileJob::Kind::Copy:
        return tr("Undo Copy (%1)").arg(items);
    case FileJob::Kind::Move:
        return tr("Undo Move (%1)").arg(items);
    case FileJob::Kind::Trash:
        return tr("Undo Move to Trash (%1)").arg(items);
    case FileJob::Kind::Restore:
        break;
    }
    return tr("Undo");
}

// Run the inverse of the last operation. Undoing a copy moves the copies to
// the trash rather than deleting them, so nothing is ever lost.
void FileOperations::undo()
{
    if (m_undo.isEmpty())
        return;
    const UndoEntry entry = m_undo.takeLast();
    emit undoChanged();

    QList<Transfer> back;
    QStringList copies;
    for (const TransferResult &r : entry.results) {
        const QFileInfo original(r.source);
        back.append({r.target, original.absolutePath(), original.fileName()});
        copies.append(r.target);
    }

    switch (entry.kind) {
    case FileJob::Kind::Copy:
        startJob(FileJob::Kind::Trash, transfersInto(copies, QString()), false);
        break;
    case FileJob::Kind::Move:
        startJob(FileJob::Kind::Move, back, false);
        break;
    case FileJob::Kind::Trash:
        startJob(FileJob::Kind::Restore, back, false);
        break;
    case FileJob::Kind::Restore:
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
