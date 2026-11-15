#pragma once

#include <QList>
#include <QString>
#include <QStringList>

// The decisions that stand between a command and the file engine (core
// code, no interface): when deleting must be asked about and in which
// words, what "Move to Trash" means where there is no Trash, which items may
// be changed, where items may be put. The window and the terminal interface
// both ask here, so the rules can't drift apart; each only shows the
// question in its own way.
namespace FileRules {

// A question to put to the user before something that can't be taken back.
// Cancel is always the default answer.
struct Question
{
    QString title;   // "Delete Permanently"
    QString text;    // "Permanently delete “a.txt”?"
    QString detail;  // "If you delete an item, it will be gone forever."
    QString confirm; // the button that goes ahead: "Delete"
};
Question permanentDelete(const QStringList &paths, bool wholeTrash = false);
Question noTrashOnServer(const QStringList &paths);

// What "Move to Trash" has to do with these items.
enum class Removal {
    Trash,           // into the Trash; can be undone
    DeleteFromTrash, // they are in the Trash already: forever, after permanentDelete()
    DeleteNoTrash,   // on a server, which has no Trash: forever, after noTrashOnServer()
};
Removal removalFor(const QStringList &paths, bool inTrash);

// May the user rename, move or trash these? Each item's folder must be
// writable, and in a sticky folder like /tmp the item must be theirs.
struct Item
{
    QString path;
    uint owner = 0;
};
bool canChange(const QList<Item> &items, bool inTrash);

// A folder that items can be copied or moved into: a real one (not Recent,
// Starred or an extension's list, not the Trash) that can be written to.
bool isTransferTarget(const QString &folder);
// Why these can't go there (they are there already); empty when they can.
QString transferProblem(const QStringList &paths, const QString &targetFolder, bool move);

} // namespace FileRules
