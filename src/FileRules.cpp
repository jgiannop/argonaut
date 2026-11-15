#include "FileRules.h"

#include "Access.h"
#include "DirectoryModel.h"
#include "Remote.h"
#include "Trash.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QHash>

namespace FileRules {

namespace {
QString tr(const char *text)
{
    return QCoreApplication::translate("FileRules", text);
}
} // namespace

Question permanentDelete(const QStringList &paths, bool wholeTrash)
{
    Question q;
    if (wholeTrash) {
        q.title = tr("Empty Trash");
        q.text = tr("Empty all items from the Trash?");
        q.detail = tr("All items in the Trash will be deleted forever.");
        q.confirm = tr("Empty Trash");
        return q;
    }
    q.title = tr("Delete Permanently");
    q.text = paths.size() == 1 ? tr("Permanently delete “%1”?").arg(QFileInfo(paths.first()).fileName())
                               : tr("Permanently delete %1 selected items?").arg(paths.size());
    q.detail = tr("If you delete an item, it will be gone forever.");
    q.confirm = tr("Delete");
    return q;
}

Question noTrashOnServer(const QStringList &paths)
{
    Question q;
    q.title = tr("No Trash on This Server");
    q.text = paths.size() == 1 ? tr("“%1” can't be moved to the Trash.").arg(QFileInfo(paths.first()).fileName())
                               : tr("These %1 items can't be moved to the Trash.").arg(paths.size());
    q.detail = tr("Files on a server can only be deleted forever.");
    q.confirm = tr("Delete Permanently");
    return q;
}

Removal removalFor(const QStringList &paths, bool inTrash)
{
    if (inTrash)
        return Removal::DeleteFromTrash; // already in the Trash: the only step left is forever
    for (const QString &path : paths) {
        if (!Remote::mountOf(path).localPath.isEmpty())
            return Removal::DeleteNoTrash; // GVfs folders have no Trash
    }
    return Removal::Trash;
}

bool canChange(const QList<Item> &items, bool inTrash)
{
    if (inTrash)
        return true; // the Trash's own folders
    QHash<QString, Access::FolderAccess> folders;
    for (const Item &item : items) {
        const int slash = int(item.path.lastIndexOf(QLatin1Char('/')));
        const QString dir = slash <= 0 ? QStringLiteral("/") : item.path.left(slash);
        if (!Access::canChangeItemsIn(dir, item.owner, &folders))
            return false;
    }
    return true;
}

bool isTransferTarget(const QString &folder)
{
    if (folder.isEmpty() || DirectoryModel::isVirtualPath(folder) || Trash::isFilesDir(folder))
        return false;
    const QFileInfo info(folder);
    return info.isDir() && info.isWritable();
}

QString transferProblem(const QStringList &paths, const QString &targetFolder, bool move)
{
    if (move && !paths.isEmpty() && QFileInfo(paths.first()).absolutePath() == targetFolder)
        return tr("They're already in that folder");
    return {};
}

} // namespace FileRules
