#pragma once

#include "Extensions.h"

#include <QList>
#include <QString>
#include <QStringList>

class QMenu;
class QWidget;

// Extensions' commands as menu entries: their submenus ("Git ▸ Switch Branch
// ▸ main"), ticks, the question a command wants asked first, the text it
// wants typed (a commit message), and the hand-over to the extension.
namespace ExtensionMenu {

// Adds the commands that fit `selection` in `folder` (empty selection: the
// folder's own commands) to `menu`. Returns how many were added. Questions
// and text fields are shown over `window`.
int add(QMenu *menu, QWidget *window, const QString &folder, const QList<Extensions::Item> &selection);

// The commands extensions want in the menu bar's `which` menu ("view"), put
// at the end of `menu` after a separator. What an earlier call added is
// taken out first, so it can be called every time the menu opens.
void addToMenuBar(QMenu *menu, QWidget *window, const QString &which, const QString &folder);

// A menu that holds nothing but one submenu shows that submenu's entries
// itself (the branch button's menu: "Git ▸ …" would be one hover too many).
void flatten(QMenu *menu);

// Asks what `command` wants asked and typed, then tells its extension.
// False if the user backed out.
bool run(QWidget *window, const Extensions::Command &command, const QString &folder, const QStringList &selection);

} // namespace ExtensionMenu
