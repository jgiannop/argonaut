// What the window does for extensions (a part of MainWindow in a file of
// its own): their commands in the menus, their results, forms and plans in
// the window the user is at, and carrying out a plan the user applied.

#include "MainWindow.h"

#include "ChooserMode.h"
#include "DirectoryModel.h"
#include "ExtensionForm.h"
#include "ExtensionMenu.h"
#include "Extensions.h"
#include "FileJob.h"
#include "FileOperations.h"
#include "PathBar.h"
#include "QuickLook.h"

#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QMenu>
#include <QPointer>

// The commands extensions offer for these items (none: for the open folder's
// empty space). The extension does the work itself, with the paths; nothing
// goes through our file operations, so Undo doesn't know about it.
void MainWindow::addExtensionCommands(QMenu *menu, const QStringList &selection)
{
    if (m_chooser || m_pane->model->isTrash())
        return;
    QList<Extensions::Item> items;
    for (const QString &path : selection) {
        const QModelIndex source = m_pane->model->indexForPath(path);
        if (!source.isValid())
            return;
        const bool dir = m_pane->model->isDir(source);
        items.append({path, dir ? QStringLiteral("inode/directory") : effectiveMimeAt(source), dir});
    }
    ExtensionMenu::add(menu, this, m_pane->model->extensionsFolder(), items);
}

void MainWindow::refreshExtensionMenus()
{
    if (m_chooser)
        return; // another program's Open/Save window has no such commands
    for (auto it = m_extensionMenus.constBegin(); it != m_extensionMenus.constEnd(); ++it)
        ExtensionMenu::addToMenuBar(it.value(), this, it.key(), m_pane->model->extensionsFolder());
}

// A click on the extension's text in the path bar (Git: the branch): the
// commands extensions have for this folder, right there.
void MainWindow::showExtensionInfoMenu(const QPoint &globalPos)
{
    if (m_chooser)
        return;
    QMenu menu(this);
    addExtensionCommands(&menu, {});
    ExtensionMenu::flatten(&menu);
    if (!menu.isEmpty())
        menu.exec(globalPos);
}

// A result an extension wants shown (a diff): in the preview pane, which
// opens for it. The next file selected takes the pane back.
void MainWindow::showExtensionResult(const QString &extension, const QString &title,
                                     const Extensions::PreviewReply &content)
{
    if (m_analyzing || m_chooser)
        return;
    if (content.update) {
        // Only replaces what is shown: it never opens the pane or takes it from a file.
        if (m_quickLook && quickLookVisible())
            m_quickLook->showResult(title.isEmpty() ? extension : title, extension, content);
        return;
    }
    if (!quickLookVisible())
        toggleQuickLook();
    if (m_quickLook)
        m_quickLook->showResult(title.isEmpty() ? extension : title, extension, content);
}

// A plan's kept moves (an extension proposed them: applyExtensionPlan): new
// folders first (undoable on their own), then one move job with renames
// ("Undo Organize" puts everything back).
void MainWindow::applyOrganizePlan(const QString &folder, const QList<QPair<QString, QPair<QString, QString>>> &moves)
{
    QList<TransferResult> created; // the top-most folders made for the plan
    QList<Transfer> transfers;
    for (const auto &m : moves) {
        const QString &item = m.first;
        const QString &sub = m.second.first;
        const QString &newName = m.second.second;
        QString dir = folder;
        for (const QString &part : sub.split(QLatin1Char('/'), Qt::SkipEmptyParts)) {
            dir += QLatin1Char('/') + part;
            if (!QFileInfo::exists(dir)) {
                if (!QDir().mkdir(dir)) {
                    showMessage(tr("Couldn't create the folder “%1”").arg(part));
                    return;
                }
                bool inside = false;
                for (const TransferResult &c : std::as_const(created))
                    inside = inside || dir.startsWith(c.target + QLatin1Char('/'));
                if (!inside)
                    created.append({QString(), dir});
            }
        }
        transfers.append({folder + QLatin1Char('/') + item, dir, newName.isEmpty() ? item : newName});
    }
    if (!created.isEmpty())
        m_ops->recordUndo(FileJob::Kind::Copy, created, tr("New Folders"));
    if (!transfers.isEmpty())
        m_ops->moveItems(transfers, tr("Organize"));
    showMessage(transfers.size() == 1 ? tr("Organizing 1 item…") : tr("Organizing %1 items…").arg(transfers.size()));
}

// ⏏ on a row an extension added (a connected server): the extension does
// it, after the same checks as for a drive when the row is a folder.
void MainWindow::ejectExtensionPlace(const QString &place, const QString &path, const QString &name)
{
    if (!Extensions::Host::isPlacePath(path) && !letGoOf(path, name))
        return;
    Extensions::Host::instance()->activatePlace(place, QStringLiteral("eject"));
}

// An extension asks the user something while it works (a login). Shown by
// one window only; the answer goes back to the extension.
// What an extension asks for or proposes is shown in one window: the one in
// front (or whose dialog is). With no window in front at all (the desktop
// has the focus), any window will do; the extension host lets one take it.
bool MainWindow::isWhereTheUserIs() const
{
    const QWidget *active = QApplication::activeWindow();
    while (active && active != this && active->parentWidget())
        active = active->parentWidget()->window();
    return !active || active == this;
}

// Claude is being asked something for an extension, which takes a while: the
// preview pane of the window in front says so until the answer is there.
void MainWindow::showClaudeWaiting(const QString &extension, const QString &name, int waiting)
{
    if (waiting <= 0) {
        if (m_quickLook)
            m_quickLook->endWorking(extension);
        return;
    }
    if (m_analyzing || m_chooser)
        return;
    if (!isWhereTheUserIs())
        return;
    if (!quickLookVisible())
        toggleQuickLook();
    if (m_quickLook)
        m_quickLook->showWorking(extension, name, tr("Waiting for Claude's answer"));
}

// Changes an extension proposes: shown in the preview pane of the window in
// front, to be ticked and applied (or dismissed) there.
void MainWindow::showExtensionPlan(const Extensions::Plan &plan)
{
    if (m_analyzing || m_chooser)
        return;
    if (!isWhereTheUserIs())
        return;
    Extensions::Host *host = Extensions::Host::instance();
    if (m_quickLook && m_quickLook->hasPlan())
        return; // this window is showing one
    if (!host->takePlan(plan.extension, plan.id))
        return; // another window has it
    if (!quickLookVisible())
        toggleQuickLook();
    if (!m_quickLook || !m_quickLook->showPlan(plan))
        host->answerPlan(plan.extension, plan.id, true);
}

// Apply was pressed. The folder may have changed since the plan was shown
// (nothing was blocked meanwhile): every kept move is checked again, as it
// is now, and only what still can be done is done, through the file engine,
// so it can be undone. The extension is told how many were.
void MainWindow::applyExtensionPlan(const Extensions::Plan &plan, const QList<Extensions::PlanMove> &kept)
{
    QList<Extensions::PlanMove> moves = kept;
    Extensions::Host::checkPlan(plan.folder, &moves);
    QList<QPair<QString, QPair<QString, QString>>> doing;
    int skipped = 0;
    for (const Extensions::PlanMove &move : std::as_const(moves)) {
        if (!move.problem.isEmpty())
            ++skipped;
        else
            doing.append({move.item, {move.folder, move.newName}});
    }
    if (!doing.isEmpty())
        applyOrganizePlan(plan.folder, doing);
    Extensions::Host::instance()->answerPlan(plan.extension, plan.id, false, int(doing.size()), skipped);
    if (skipped > 0) {
        showMessage(doing.isEmpty() ? tr("Nothing was moved: the folder has changed since the plan was made")
                                    : tr("Moving %1 items; %2 were left where they are: the folder has changed since the plan was made")
                                          .arg(doing.size())
                                          .arg(skipped));
    }
}

void MainWindow::showExtensionForm(const Extensions::Form &form)
{
    if (!isWhereTheUserIs())
        return;
    Extensions::Host *host = Extensions::Host::instance();
    if (!host->takeForm(form.extension, form.id))
        return; // another window has it
    QPointer<MainWindow> self(this);
    const ExtensionForm::Answer answer = ExtensionForm::ask(this, form);
    host->answerForm(form.extension, form.id, answer.cancelled, answer.values, answer.button);
    Q_UNUSED(self);
}
