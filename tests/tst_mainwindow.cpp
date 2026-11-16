// Tests for the window's own decisions about what gets trashed, deleted,
// moved or returned: the code between the user and the file engine (which
// tst_fileoperations covers). They drive the real MainWindow, headless.
//
// Everything is sandboxed: HOME, the XDG folders and the trash live in a
// temporary folder, real drives' trash folders are never looked at
// (ARGONAUT_TRASH_HOME_ONLY), and `gio` / the eject program are fake scripts
// that only write down how they were called.

#include "DirectoryModel.h"
#include "ExtensionMenu.h"
#include "ExtensionsDialog.h"
#include "FadeOverlay.h"
#include "FileOperations.h"
#include "AnalyzeView.h"
#include "Bookmarks.h"
#include "DiskUsageModel.h"
#include "InlineRename.h"
#include "MainWindow.h"
#include "MountActions.h"
#include "PathBar.h"
#include "QuickLook.h"
#include "RichTextStyle.h"
#include "Theme.h"
#include "Sidebar.h"
#include "TabStrip.h"
#if defined(ARGONAUT_TERMINAL)
#include "TerminalWidget.h"
#endif
#include "TestSupport.h"
#include "Trash.h"

#if defined(ARGONAUT_DBUS)
#include "ChooserBar.h"
#include "CommandPalette.h"
#include "ChooserMode.h"
#include "FileChooserPortal.h"
#include "FileChooserRequest.h"
#endif

#include <QAbstractButton>
#include <QAction>
#include <QCheckBox>
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPointer>
#include <QComboBox>
#include <QDialog>
#include <QSignalSpy>
#include <QSplitter>
#include <QTemporaryDir>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextTable>
#include <QTimer>
#include <QUrl>
#include <QtTest>

#include <memory>

#include <unistd.h>

class TestMainWindow : public QObject
{
    Q_OBJECT

public:
    explicit TestMainWindow(const QString &sandbox) : m_sandbox(sandbox) {}

private slots:
    void init();
    void cleanup();

    void trashTakesOnlyTheSelectionAndUndoRestores();
    void permanentDeleteAsksFirst();
    void deleteInsideTheTrashIsConfirmed();
    void emptyTrashAsksAndTouchesOnlyTheTrash();
    void analyzeViewTurnsFileCommandsOff();
    void analyzeViewMeasuresAndTrashesOnlyItsRow();
    void readOnlyFolderOnlyAllowsCopy();
    void cutIsPastedOnce();
    void dropRules();
    void dropOnTheTrashTrashes();
    void splitViewActsOnThePaneYouAreIn();
    void showHiddenAppliesToBothPanes();
    void splitPanesKeepTheirOwnFolderViewAndHistory();
    void otherPaneWorksInTheBackground();
    void copyToOtherPane();
    void tabsOfPlacesHaveTheirNames();
    void tabsRememberFolderSelectionAndHistory();
    void inlineRenameRenamesAndRefusesTakenNames();
    void ejectLeavesTheDriveAndRunsTheTool();
    void sidebarListsPlacesAndFollowsBookmarks();
    void previewIsAPaneThatFollowsTheSelection();
    void extensionCommandsAskFirst();
    void extensionsWindowListsAndSwitches();
    void fadeNeverGetsInTheWay();
    void previewFromAnExtension();
    void richTextGetsAPageLook();
    void terminalBringsTheFilesAlong();
    void extensionMenusAskAndShow();
    void diffIsColoured();
    void extensionListIsAPlace();
    void extensionResultStaysLive();
    void longPictureFromAnExtensionScrolls();
    void extensionPlacesInTheSidebar();
    void extensionFormsAndAddresses();
    void themesFromFiles();
#if defined(ARGONAUT_DBUS)
    void chooserOpenReturnsTheSelection();
    void chooserSaveAsksBeforeReplacing();
    void chooserCancelAnswersOnce();
#endif

private:
    QString make(const QString &relative, const QByteArray &content = "x");
    bool exists(const QString &path) const { return QFileInfo::exists(path); }
    QString inTrash(const QString &name) const { return Trash::homeFilesDir() + '/' + name; }
    void open(const QString &folder);
    void select(const QStringList &paths);
    // Runs `action` and waits for the file job it starts to finish.
    void runJob(const std::function<void()> &action);
    // Presses `button` in the next question the window asks. `m_asked` counts
    // the questions, `m_question` keeps the last one's text.
    void answerNext(const QString &button);

    QString m_sandbox;
    QString m_dir; // this test's folder
    int m_counter = 0;
    std::unique_ptr<MainWindow> w;
    QPointer<QTimer> m_answer;
    int m_asked = 0;
    QString m_question;
};

void TestMainWindow::init()
{
    m_dir = m_sandbox + QStringLiteral("/home/work%1").arg(++m_counter);
    QVERIFY(QDir().mkpath(m_dir));
    QDir(Trash::homeFilesDir()).removeRecursively();
    QDir(QFileInfo(Trash::homeFilesDir()).absolutePath() + "/info").removeRecursively();
    m_asked = 0;
    m_question.clear();
    w = std::make_unique<MainWindow>();
    w->resize(1000, 700);
    w->show();
}

void TestMainWindow::cleanup()
{
    delete m_answer;
    // A question nobody answered would block the next test: fail loudly instead.
    if (QWidget *modal = QApplication::activeModalWidget()) {
        modal->close();
        QFAIL("a question was left open");
    }
    w.reset();
    // Folders made read-only by a test must be removable again.
    QFile::setPermissions(m_dir + "/locked", QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
}

QString TestMainWindow::make(const QString &relative, const QByteArray &content)
{
    const QString path = m_dir + '/' + relative;
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        qFatal("couldn't write %s", qPrintable(path));
    f.write(content);
    return path;
}

void TestMainWindow::open(const QString &folder)
{
    QSignalSpy loaded(w->m_pane->model, &DirectoryModel::loaded);
    QVERIFY(w->navigateTo(folder));
    QTRY_VERIFY_WITH_TIMEOUT(!loaded.isEmpty(), 10000);
}

void TestMainWindow::select(const QStringList &paths)
{
    w->selectPaths(paths);
    QStringList sorted = paths;
    sorted.sort();
    QTRY_VERIFY_WITH_TIMEOUT([&] {
        QStringList now = w->selectedPaths();
        now.sort();
        return now == sorted;
    }(), 5000);
}

void TestMainWindow::runJob(const std::function<void()> &action)
{
    QSignalSpy finished(w->m_ops, &FileOperations::jobFinished);
    action();
    QTRY_VERIFY_WITH_TIMEOUT(!finished.isEmpty(), 15000);
    QTest::qWait(50); // let the window react (selection, toast, undo menu)
}

void TestMainWindow::answerNext(const QString &button)
{
    delete m_answer;
    m_answer = new QTimer(this);
    m_answer->setInterval(10);
    connect(m_answer, &QTimer::timeout, this, [this, button] {
        auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
        if (!box)
            return;
        m_answer->stop();
        ++m_asked;
        m_question = box->windowTitle() + QLatin1Char(' ') + box->text();
        const auto buttons = box->buttons();
        for (QAbstractButton *b : buttons) {
            if (b->text().remove(QLatin1Char('&')) == button) {
                b->click();
                return;
            }
        }
        qWarning("no button \"%s\" in the question \"%s\"", qPrintable(button), qPrintable(m_question));
        box->reject();
    });
    m_answer->start();
}

// Delete: the selected item goes to the Trash, its neighbours stay, and
// Ctrl+Z brings it back.
void TestMainWindow::trashTakesOnlyTheSelectionAndUndoRestores()
{
    const QString a = make("a.txt"), b = make("b.txt"), c = make("c.txt");
    make("folder/inside.txt");
    open(m_dir);
    select({b});
    QVERIFY(w->m_trashAct->isEnabled());

    runJob([&] { w->m_trashAct->trigger(); });
    QVERIFY(!exists(b));
    QVERIFY(exists(a) && exists(c) && exists(m_dir + "/folder/inside.txt"));
    QVERIFY(exists(inTrash("b.txt")));
    QCOMPARE(m_asked, 0);

    QVERIFY(w->m_undoAct->isEnabled());
    runJob([&] { w->m_undoAct->trigger(); });
    QVERIFY(exists(b));
    QVERIFY(!exists(inTrash("b.txt")));
}

// Shift+Delete always asks, Cancel is the default and changes nothing, and
// "Delete" removes the selection only (not through the Trash).
void TestMainWindow::permanentDeleteAsksFirst()
{
    const QString a = make("a.txt"), b = make("b.txt");
    open(m_dir);
    select({b});
    QVERIFY(w->m_deleteAct->isEnabled());

    answerNext("Cancel");
    w->m_deleteAct->trigger();
    QCOMPARE(m_asked, 1);
    QVERIFY(m_question.contains("b.txt"));
    QTest::qWait(100);
    QVERIFY(exists(a) && exists(b));

    answerNext("Delete");
    runJob([&] { w->m_deleteAct->trigger(); });
    QCOMPARE(m_asked, 2);
    QVERIFY(!exists(b));
    QVERIFY(exists(a));
    QVERIFY(!exists(inTrash("b.txt")));
}

// Delete inside the Trash is the permanent kind, so it must ask too.
void TestMainWindow::deleteInsideTheTrashIsConfirmed()
{
    const QString a = make("a.txt"), b = make("b.txt");
    open(m_dir);
    select({a, b});
    runJob([&] { w->m_trashAct->trigger(); });
    QVERIFY(exists(inTrash("a.txt")) && exists(inTrash("b.txt")));

    open(Trash::homeFilesDir());
    QVERIFY(w->m_pane->model->isTrash());
    select({inTrash("a.txt")});

    answerNext("Cancel");
    w->m_trashAct->trigger();
    QCOMPARE(m_asked, 1);
    QTest::qWait(100);
    QVERIFY(exists(inTrash("a.txt")));

    answerNext("Delete");
    runJob([&] { w->m_trashAct->trigger(); });
    QVERIFY(!exists(inTrash("a.txt")));
    QVERIFY(exists(inTrash("b.txt"))); // not selected
}

// Empty Trash asks, and deletes what is in the Trash: never the open folder
// or its selection.
void TestMainWindow::emptyTrashAsksAndTouchesOnlyTheTrash()
{
    const QString keep = make("keep.txt"), gone = make("gone.txt");
    make("folder/inside.txt");
    open(m_dir);
    select({gone});
    runJob([&] { w->m_trashAct->trigger(); });
    QVERIFY(exists(inTrash("gone.txt")));
    select({keep, m_dir + "/folder"}); // selected, but Empty Trash isn't about them

    answerNext("Cancel");
    w->emptyTrash();
    QCOMPARE(m_asked, 1);
    QTest::qWait(100);
    QVERIFY(exists(inTrash("gone.txt")));

    answerNext("Empty Trash");
    runJob([&] { w->emptyTrash(); });
    QCOMPARE(m_asked, 2);
    QVERIFY(!exists(inTrash("gone.txt")));
    QVERIFY(exists(keep) && exists(m_dir + "/folder/inside.txt"));

    // Nothing left: no question, nothing to do.
    w->emptyTrash();
    QCOMPARE(m_asked, 2);
}

// In the Analyze view the file view is hidden but keeps its selection:
// Delete and friends must not act on what you can't see.
void TestMainWindow::analyzeViewTurnsFileCommandsOff()
{
    const QString a = make("a.txt");
    open(m_dir);
    select({a});
    QVERIFY(w->m_trashAct->isEnabled());

    w->m_analyzeAct->trigger();
    QTRY_VERIFY(w->m_analyzing);
    QVERIFY(w->selectedPaths().isEmpty());
    for (QAction *action : {w->m_trashAct, w->m_deleteAct, w->m_cutAct, w->m_renameAct})
        QVERIFY2(!action->isEnabled(), qPrintable(action->text()));
    w->m_trashAct->trigger();
    w->trashSelection(); // even called directly
    QTest::qWait(200);
    QVERIFY(exists(a));
    QCOMPARE(m_asked, 0);
}

// The Analyze page has its own selection: Delete there trashes the row
// chosen there (never the folder being analysed itself), and the tree stays
// right afterwards without measuring again.
void TestMainWindow::analyzeViewMeasuresAndTrashesOnlyItsRow()
{
    make("big/data.bin", QByteArray(400 * 1000, 'x'));
    make("small/note.txt");
    const QString keep = make("keep.txt");
    open(m_dir);
    select({keep}); // selected in the file view: must not matter below

    w->m_analyzeAct->trigger();
    AnalyzeView *view = w->m_analyzeView;
    QTRY_VERIFY_WITH_TIMEOUT(view->statusText().contains("on disk"), 10000);
    auto *usage = qobject_cast<DiskUsageModel *>(view->model());
    QVERIFY(usage);
    const QModelIndex big = usage->indexForPath(m_dir + "/big");
    QVERIFY(big.isValid());
    view->setCurrentIndex(big);
    QCOMPARE(view->selectedRow().path, m_dir + "/big");
    QVERIFY(!view->selectedRow().isFile);
    QVERIFY(view->statusText().contains("big")); // the row's share

    QAction *trash = view->actions().value(0);
    QVERIFY(trash);
    runJob([&] { trash->trigger(); });
    QVERIFY(!exists(m_dir + "/big"));
    QVERIFY(exists(inTrash("big")));
    QVERIFY(exists(keep) && exists(m_dir + "/small/note.txt"));
    QTRY_VERIFY(!usage->indexForPath(m_dir + "/big").isValid()); // gone from the tree too
    QVERIFY(!view->isEmpty());

    // The top row is the folder being analysed: Delete does nothing to it.
    view->setCurrentIndex(usage->index(0, 0));
    QCOMPARE(view->selectedRow().path, m_dir);
    trash->trigger();
    QTest::qWait(200);
    QVERIFY(exists(m_dir) && exists(keep));

    // Biggest files as rows of their own, with the open folders kept open.
    runJob([&] { w->m_undoAct->trigger(); });
    QTRY_VERIFY(usage->indexForPath(m_dir + "/big").isValid());
    view->expand(usage->indexForPath(m_dir + "/big"));
    view->setShowFiles(true);
    QVERIFY(view->isExpanded(usage->indexForPath(m_dir + "/big")));
    const QModelIndex file = usage->indexForPath(m_dir + "/big/data.bin");
    QVERIFY(file.isValid());
    view->setCurrentIndex(file);
    QVERIFY(view->selectedRow().isFile);
}

// Items in a folder you can't change: Copy works, nothing that would move or
// remove them does.
void TestMainWindow::readOnlyFolderOnlyAllowsCopy()
{
    if (geteuid() == 0)
        QSKIP("root can write everywhere");
    const QString file = make("locked/a.txt");
    QVERIFY(QFile::setPermissions(m_dir + "/locked", QFile::ReadOwner | QFile::ExeOwner));
    open(m_dir + "/locked");
    select({file});

    QVERIFY(w->m_copyAct->isEnabled());
    for (QAction *action : {w->m_trashAct, w->m_deleteAct, w->m_cutAct, w->m_renameAct, w->m_pasteAct})
        QVERIFY2(!action->isEnabled(), qPrintable(action->text()));
    // And it can't be dropped into.
    QCOMPARE(w->dropActionFor({make("b.txt")}, m_dir + "/locked", Qt::NoModifier, Qt::CopyAction | Qt::MoveAction),
             Qt::IgnoreAction);
}

// Cut + Paste moves, and the same cut can't be pasted a second time.
void TestMainWindow::cutIsPastedOnce()
{
    const QString a = make("from/a.txt");
    QDir().mkpath(m_dir + "/to");
    open(m_dir + "/from");
    select({a});
    w->m_cutAct->trigger();
    QVERIFY(exists(a)); // nothing happens until Paste

    open(m_dir + "/to");
    QTRY_VERIFY(w->m_pasteAct->isEnabled());
    runJob([&] { w->m_pasteAct->trigger(); });
    QVERIFY(!exists(a));
    QVERIFY(exists(m_dir + "/to/a.txt"));
    QTRY_VERIFY(!w->m_pasteAct->isEnabled());

    // Undo moves it back.
    runJob([&] { w->m_undoAct->trigger(); });
    QVERIFY(exists(a));
    QVERIFY(!exists(m_dir + "/to/a.txt"));
}

void TestMainWindow::dropRules()
{
    const QString file = make("a.txt");
    const QString folder = m_dir + "/folder";
    const QString other = m_dir + "/other";
    make("folder/sub/x.txt");
    QDir().mkpath(other);
    const Qt::DropActions both = Qt::CopyAction | Qt::MoveAction;

    // Same drive: move. Ctrl copies, Shift moves.
    QCOMPARE(w->dropActionFor({file}, other, Qt::NoModifier, both), Qt::MoveAction);
    QCOMPARE(w->dropActionFor({file}, other, Qt::ControlModifier, both), Qt::CopyAction);
    QCOMPARE(w->dropActionFor({file}, other, Qt::ShiftModifier, both), Qt::MoveAction);
    // Where it already is: a move would change nothing, a copy duplicates.
    QCOMPARE(w->dropActionFor({file}, m_dir, Qt::NoModifier, both), Qt::IgnoreAction);
    QCOMPARE(w->dropActionFor({file}, m_dir, Qt::ControlModifier, both), Qt::CopyAction);
    // A folder into itself or into a folder inside it: never.
    QCOMPARE(w->dropActionFor({folder}, folder, Qt::NoModifier, both), Qt::IgnoreAction);
    QCOMPARE(w->dropActionFor({folder}, folder + "/sub", Qt::NoModifier, both), Qt::IgnoreAction);
    QCOMPARE(w->dropActionFor({folder}, folder + "/sub", Qt::ControlModifier, both), Qt::IgnoreAction);
    // A folder whose name only starts the same is another folder.
    QDir().mkpath(m_dir + "/folder2");
    QCOMPARE(w->dropActionFor({folder}, m_dir + "/folder2", Qt::NoModifier, both), Qt::MoveAction);
    // Nothing, nowhere, or a place that doesn't exist.
    QCOMPARE(w->dropActionFor({}, other, Qt::NoModifier, both), Qt::IgnoreAction);
    QCOMPARE(w->dropActionFor({file}, QString(), Qt::NoModifier, both), Qt::IgnoreAction);
    QCOMPARE(w->dropActionFor({file}, m_dir + "/missing", Qt::NoModifier, both), Qt::IgnoreAction);
    // The source only offers a copy: never turn it into a move.
    QCOMPARE(w->dropActionFor({file}, other, Qt::NoModifier, Qt::CopyAction), Qt::CopyAction);
    QCOMPARE(w->dropActionFor({file}, other, Qt::ShiftModifier, Qt::CopyAction), Qt::CopyAction);

    // The Trash: a move, whatever the keys; not for what is already in it.
    QDir().mkpath(Trash::homeFilesDir());
    QCOMPARE(w->dropActionFor({file}, Trash::homeFilesDir(), Qt::ControlModifier, both), Qt::MoveAction);
    QCOMPARE(w->dropActionFor({file}, Trash::homeFilesDir(), Qt::NoModifier, Qt::CopyAction), Qt::IgnoreAction);
    QFile(inTrash("old.txt")).open(QIODevice::WriteOnly);
    QCOMPARE(w->dropActionFor({inTrash("old.txt")}, Trash::homeFilesDir(), Qt::NoModifier, both), Qt::IgnoreAction);
}

// A drop on the Trash is "Move to Trash" (restorable, undoable), not a plain
// move of the file into the trash folder.
void TestMainWindow::dropOnTheTrashTrashes()
{
    const QString a = make("a.txt"), b = make("b.txt");
    open(m_dir);
    QDir().mkpath(Trash::homeFilesDir());

    runJob([&] { w->performDrop({a}, Trash::homeFilesDir(), Qt::MoveAction); });
    QVERIFY(!exists(a));
    QVERIFY(exists(b));
    QVERIFY(exists(inTrash("a.txt")));
    QVERIFY(exists(QFileInfo(Trash::homeFilesDir()).absolutePath() + "/info/a.txt.trashinfo"));

    runJob([&] { w->m_undoAct->trigger(); });
    QVERIFY(exists(a));
}

// With two panes, commands act on the pane you are in, not on the other
// pane's selection.
void TestMainWindow::splitViewActsOnThePaneYouAreIn()
{
    const QString left = make("left/l.txt");
    const QString right = make("right/r.txt");
    make("right/stay.txt");
    open(m_dir + "/left");
    select({left});

    w->setSplitView(true);
    w->activatePane(1);
    open(m_dir + "/right");
    select({right});
    QCOMPARE(w->m_activePane, 1);
    QCOMPARE(w->selectedPaths(), QStringList{right});

    runJob([&] { w->m_trashAct->trigger(); });
    QVERIFY(!exists(right));
    QVERIFY(exists(left)); // selected in the other pane: untouched
    QVERIFY(exists(m_dir + "/right/stay.txt"));

    // Back in the first pane, its own selection is the one that counts.
    w->activatePane(0);
    QCOMPARE(w->selectedPaths(), QStringList{left});
    runJob([&] { w->m_trashAct->trigger(); });
    QVERIFY(!exists(left));
    QVERIFY(exists(m_dir + "/right/stay.txt"));
}

// Each pane owns its folder, history, view mode and selection; the header
// shows the pane you are in.
void TestMainWindow::splitPanesKeepTheirOwnFolderViewAndHistory()
{
    const QString l = make("left/l.txt");
    make("left/deep/x.txt");
    const QString r = make("right/r.txt");
    open(m_dir + "/left");
    w->m_listAct->trigger(); // the first pane as a list
    QVERIFY(!w->m_pane->grid);

    w->setSplitView(true);
    QCOMPARE(w->m_paneCount, 2);
    QCOMPARE(w->m_activePane, 1);
    QCOMPARE(w->m_pane, &w->m_panes[1]);
    QVERIFY(!w->inBackground());
    // The new pane starts like the first: same folder, same kind of view.
    QCOMPARE(w->m_panes[1].current, m_dir + "/left");
    QVERIFY(!w->m_panes[1].grid);
    QVERIFY(w->m_panes[1].model != w->m_panes[0].model);
    QVERIFY(w->m_panes[1].back.isEmpty()); // but its own history

    open(m_dir + "/right");
    w->m_gridAct->trigger(); // the second pane as a grid
    QTRY_VERIFY(w->m_panes[1].model->indexForPath(r).isValid());
    select({r});
    QCOMPARE(w->m_panes[0].current, m_dir + "/left"); // untouched
    QVERIFY(!w->m_panes[0].grid);
    QVERIFY(w->m_panes[1].grid);
    QVERIFY(w->m_backAct->isEnabled()); // pane 2 came from "left"
    QCOMPARE(w->m_pathBar->path(), m_dir + "/right");

    // Back in the first pane: its folder in the header, its own (empty) way back.
    w->activatePane(0);
    QCOMPARE(w->m_pane, &w->m_panes[0]);
    QCOMPARE(w->m_pane->current, m_dir + "/left");
    QCOMPARE(w->m_pathBar->path(), m_dir + "/left");
    QVERIFY(w->m_listAct->isChecked());
    open(m_dir + "/left/deep");
    QCOMPARE(w->m_panes[1].current, m_dir + "/right");
    QCOMPARE(w->m_panes[1].back.size(), 1);
    QCOMPARE(w->m_panes[0].back.size(), 1); // left -> deep

    // The second pane kept its selection all along.
    w->activatePane(1);
    QCOMPARE(w->selectedPaths(), QStringList{r});
    QVERIFY(w->m_gridAct->isChecked());

    // Split off: back to one pane, the first, as it was.
    w->setSplitView(false);
    QCOMPARE(w->m_activePane, 0);
    QCOMPARE(w->m_pane->current, m_dir + "/left/deep");
    QCOMPARE(w->m_pathBar->path(), m_dir + "/left/deep");
    QVERIFY(exists(l) && exists(r));
}

// What happens in the pane you are not in (its folder changes, files appear)
// is handled there, and leaves your pane and the header alone.
void TestMainWindow::otherPaneWorksInTheBackground()
{
    make("left/l.txt");
    make("right/r.txt");
    open(m_dir + "/left");
    w->setSplitView(true);
    open(m_dir + "/right");
    w->activatePane(0);
    select({m_dir + "/left/l.txt"});
    const QString header = w->m_pathBar->path();
    const QString title = w->windowTitle();

    // A file appears in the other pane's folder: its model picks it up.
    const QString added = make("right/new.txt");
    QTRY_VERIFY_WITH_TIMEOUT(w->m_panes[1].model->indexForPath(added).isValid(), 5000);
    QCOMPARE(w->m_pane, &w->m_panes[0]); // still working on the user's pane
    QVERIFY(!w->inBackground());
    QCOMPARE(w->m_pathBar->path(), header);
    QCOMPARE(w->windowTitle(), title);
    QCOMPARE(w->selectedPaths(), QStringList{m_dir + "/left/l.txt"});

    // The other pane's folder disappears: that pane leaves it, yours stays.
    QDir(m_dir + "/right").removeRecursively();
    QTRY_VERIFY_WITH_TIMEOUT(w->m_panes[1].current != m_dir + "/right", 5000);
    QCOMPARE(w->m_panes[0].current, m_dir + "/left");
    QCOMPARE(w->m_pathBar->path(), header);
    QCOMPARE(w->m_pane, &w->m_panes[0]);
}

// F5: the selection is copied into the other pane's folder, through the
// normal engine (so it can be undone), and shown selected there.
void TestMainWindow::copyToOtherPane()
{
    const QString a = make("left/a.txt");
    QDir().mkpath(m_dir + "/right");
    open(m_dir + "/left");
    w->setSplitView(true);
    open(m_dir + "/right");
    w->activatePane(0);
    select({a});
    QTRY_VERIFY(w->m_copyToOtherAct->isEnabled());

    runJob([&] { w->m_copyToOtherAct->trigger(); });
    QVERIFY(exists(a));
    QVERIFY(exists(m_dir + "/right/a.txt"));
    // The copy is selected in the pane it went to; yours keeps its selection.
    QTRY_VERIFY(w->m_panes[1].model->indexForPath(m_dir + "/right/a.txt").isValid());
    QCOMPARE(w->selectedPaths(), QStringList{a});
    {
        w->activatePane(1);
        QTRY_COMPARE(w->selectedPaths(), QStringList{m_dir + "/right/a.txt"});
        w->activatePane(0);
    }
    runJob([&] { w->m_undoAct->trigger(); });
    QVERIFY(!exists(m_dir + "/right/a.txt"));
    QVERIFY(exists(a));
}

// "Show Hidden Files" is one switch for the window: both panes follow it.
void TestMainWindow::showHiddenAppliesToBothPanes()
{
    const QString left = make("left/.secret");
    const QString right = make("right/.secret");
    make("left/a.txt");
    make("right/b.txt");
    if (w->m_hiddenAct->isChecked())
        w->m_hiddenAct->setChecked(false);
    open(m_dir + "/left");
    w->setSplitView(true);
    w->activatePane(1);
    open(m_dir + "/right");
    QVERIFY(!w->m_panes[0].model->indexForPath(left).isValid());
    QVERIFY(!w->m_panes[1].model->indexForPath(right).isValid());

    w->m_hiddenAct->setChecked(true); // while in the second pane
    QTRY_VERIFY(w->m_panes[1].model->indexForPath(right).isValid());
    QTRY_VERIFY_WITH_TIMEOUT(w->m_panes[0].model->indexForPath(left).isValid(), 3000);

    w->activatePane(0);
    w->m_hiddenAct->setChecked(false);
    QTRY_VERIFY(!w->m_panes[0].model->indexForPath(left).isValid());
    QTRY_VERIFY_WITH_TIMEOUT(!w->m_panes[1].model->indexForPath(right).isValid(), 3000);
}

// A tab shows the place's name, as the window title does: not "recent:///".
void TestMainWindow::tabsOfPlacesHaveTheirNames()
{
    QCOMPARE(w->titleFor(DirectoryModel::recentPath()), QStringLiteral("Recent"));
    QDir().mkpath(Trash::homeFilesDir());
    open(m_dir);
    w->openTab(DirectoryModel::recentPath(), false);
    w->openTab(Trash::homeFilesDir(), false);
    QCOMPARE(w->m_tabBar->count(), 3);
    QStringList titles;
    for (int i = 0; i < w->m_tabBar->count(); ++i)
        titles << w->m_tabBar->tabText(i);
    QCOMPARE(titles, (QStringList{QFileInfo(m_dir).fileName(), "Trash", "Recent"})); // newest right after the active tab
    // And the tab really opens the place.
    w->m_tabBar->activate(2);
    QTRY_COMPARE(w->m_pane->current, DirectoryModel::recentPath());
}

// Tabs share the one view: leaving a tab saves its folder, selection and
// history, coming back restores them, and a closed tab can be brought back.
void TestMainWindow::tabsRememberFolderSelectionAndHistory()
{
    const QString a = make("one/a.txt");
    make("one/sub/x.txt");
    const QString b = make("two/b.txt");
    TabStrip *tabs = w->m_tabBar;
    QVERIFY(!tabs->isVisibleTo(w.get())); // one tab: no strip

    open(m_dir + "/one");
    open(m_dir + "/one/sub");
    w->m_backAct->trigger(); // back in "one", with somewhere to go forward to
    QTRY_COMPARE(w->m_pane->current, m_dir + "/one");
    QTRY_VERIFY(w->m_pane->model->indexForPath(a).isValid());
    select({a});

    w->openTab(m_dir + "/two", true);
    QTRY_COMPARE(w->m_pane->current, m_dir + "/two");
    QCOMPARE(tabs->count(), 2);
    QCOMPARE(tabs->activeIndex(), 1);
    QVERIFY(tabs->isVisibleTo(w.get()));
    QTRY_VERIFY(w->m_pane->model->indexForPath(b).isValid());
    select({b});
    QVERIFY(!w->m_forwardAct->isEnabled()); // the new tab has its own, empty history

    tabs->step(-1); // Ctrl+Shift+Tab
    QTRY_COMPARE(w->m_pane->current, m_dir + "/one");
    QTRY_COMPARE(w->selectedPaths(), QStringList{a});
    QVERIFY(w->m_forwardAct->isEnabled());
    QCOMPARE(tabs->tabText(0), QStringLiteral("one"));
    QCOMPARE(tabs->pathAt(1), m_dir + "/two");

    // Close the second tab from the first, then bring it back as it was.
    tabs->closeTab(1);
    QCOMPARE(tabs->count(), 1);
    QCOMPARE(w->m_pane->current, m_dir + "/one");
    QVERIFY(!tabs->isVisibleTo(w.get()));
    tabs->reopenClosed();
    QCOMPARE(tabs->count(), 2);
    QTRY_COMPARE(w->m_pane->current, m_dir + "/two");
    QTRY_COMPARE(w->selectedPaths(), QStringList{b});

    // Closing the active tab shows a neighbour; files are never touched.
    tabs->closeActive();
    QTRY_COMPARE(w->m_pane->current, m_dir + "/one");
    QVERIFY(exists(a) && exists(b));
}

// F2: Enter renames (undoable), a taken or forbidden name keeps the field
// open and changes nothing, Esc gives up.
void TestMainWindow::inlineRenameRenamesAndRefusesTakenNames()
{
    const QString a = make("report.tar.gz"), b = make("taken.txt");
    open(m_dir);
    select({a});
    QVERIFY(w->m_renameAct->isEnabled());
    w->m_renameAct->trigger();
    QTRY_VERIFY(w->m_rename->isActive());
    auto *edit = w->m_pane->view->viewport()->findChild<QLineEdit *>("renameEdit");
    QVERIFY(edit);
    QCOMPARE(edit->text(), QStringLiteral("report.tar.gz"));
    QCOMPARE(edit->selectedText(), QStringLiteral("report")); // the name, not ".tar.gz"

    // A name another item has: refused, the field stays.
    edit->setText("taken.txt");
    QVERIFY(!w->m_rename->commit());
    QVERIFY(w->m_rename->isActive());
    QVERIFY(exists(a) && exists(b));
    // A name with a slash: refused too.
    edit->setText("sub/name");
    QVERIFY(!w->m_rename->commit());
    QVERIFY(exists(a));

    // Enter renames, selects the item under its new name, and can be undone.
    edit->setText("final.tar.gz");
    QTest::keyClick(edit, Qt::Key_Return);
    QVERIFY(!w->m_rename->isActive());
    QVERIFY(!exists(a));
    QVERIFY(exists(m_dir + "/final.tar.gz"));
    QTRY_COMPARE(w->selectedPaths(), QStringList{m_dir + "/final.tar.gz"});
    runJob([&] { w->m_undoAct->trigger(); });
    QVERIFY(exists(a));

    // Esc changes nothing.
    QTRY_VERIFY(w->m_pane->model->indexForPath(a).isValid());
    select({a});
    w->m_renameAct->trigger();
    QTRY_VERIFY(w->m_rename->isActive());
    edit = w->m_pane->view->viewport()->findChild<QLineEdit *>("renameEdit");
    QVERIFY(edit);
    edit->setText("never.txt");
    QTest::keyClick(edit, Qt::Key_Escape);
    QVERIFY(!w->m_rename->isActive());
    QVERIFY(exists(a) && !exists(m_dir + "/never.txt"));
}

// Eject: the window first leaves the drive (or it would keep it busy
// itself), then the system's tool is asked to eject it. Here the tool is the
// fake program, which writes down how it was called.
void TestMainWindow::ejectLeavesTheDriveAndRunsTheTool()
{
    const QString drive = m_dir + "/stick";
    make("stick/photos/a.jpg");
    const QString log = m_sandbox + "/fake-program.log";
    QFile::remove(log);
    open(drive + "/photos");

    QSignalSpy messages(w->m_mounts, &MountActions::message);
    QSignalSpy changed(w->m_mounts, &MountActions::mountsChanged);
    w->ejectDrive(drive, "Stick");
    QTRY_COMPARE(w->m_pane->current, QDir::homePath());
    QTRY_VERIFY_WITH_TIMEOUT(!changed.isEmpty(), 5000); // the tool finished: the sidebar is rebuilt
    QFile f(log);
    QVERIFY(f.open(QIODevice::ReadOnly));
    QVERIFY(f.readAll().contains(("mount --eject " + drive).toUtf8()));
    QVERIFY(messages.last().first().toString().contains("safely removed"));
    QVERIFY(exists(drive + "/photos/a.jpg")); // ejecting never deletes
}

// The sidebar builds its own rows, follows the bookmarks file, and shows
// which row is the open folder.
void TestMainWindow::sidebarListsPlacesAndFollowsBookmarks()
{
    Sidebar *sidebar = w->m_sidebar;
    auto rowOf = [sidebar](const QString &path) -> QListWidgetItem * {
        for (int i = 0; i < sidebar->count(); ++i)
            if (Sidebar::pathOf(sidebar->item(i)) == path)
                return sidebar->item(i);
        return nullptr;
    };
    QVERIFY(rowOf(DirectoryModel::recentPath()));
    QVERIFY(rowOf(DirectoryModel::starredPath()));
    QVERIFY(rowOf(QDir::homePath()));
    QVERIFY(rowOf(Trash::homeFilesDir()));
    QCOMPARE(Sidebar::kindOf(rowOf(QDir::homePath())), Sidebar::PlaceItem);
    QVERIFY(!Sidebar::canEject(rowOf(QDir::homePath())));
    // Separators have no path and can't be chosen.
    int separators = 0;
    for (int i = 0; i < sidebar->count(); ++i) {
        if (Sidebar::isSeparator(sidebar->item(i))) {
            ++separators;
            QVERIFY(Sidebar::pathOf(sidebar->item(i)).isEmpty());
            QVERIFY(!(sidebar->item(i)->flags() & Qt::ItemIsSelectable));
        }
    }
    QVERIFY(separators >= 1);

    // A bookmark added by anyone (here: straight into the shared file)
    // appears by itself, as a bookmark row.
    const QString folder = m_dir + "/project";
    QDir().mkpath(folder);
    QVERIFY(!rowOf(folder));
    QSignalSpy refreshed(sidebar, &Sidebar::refreshed);
    QVERIFY(Bookmarks::add(folder));
    QTRY_VERIFY_WITH_TIMEOUT(rowOf(folder), 5000);
    QVERIFY(!refreshed.isEmpty());
    QCOMPARE(Sidebar::kindOf(rowOf(folder)), Sidebar::BookmarkItem);

    // The open folder's row is the selected one; a folder without a row selects none.
    open(folder);
    QTRY_COMPARE(sidebar->currentItem(), rowOf(folder));
    open(m_dir);
    QTRY_VERIFY(!sidebar->currentItem() || !sidebar->currentItem()->isSelected());

    Bookmarks::remove(folder);
    QTRY_VERIFY_WITH_TIMEOUT(!rowOf(folder), 5000);
}

#if defined(ARGONAUT_DBUS)

// Open: "opening" a file answers the app with it; nothing is launched, moved
// or deleted, and the commands that would change files or open other windows
// stay off.
void TestMainWindow::chooserOpenReturnsTheSelection()
{
    const QString a = make("a.txt"), b = make("b.txt");
    ChooserRequest request;
    request.mode = ChooserRequest::Mode::Open;
    request.appId = "org.example.Test";
    request.title = "Open a file";
    request.multiple = true;
    request.currentFolder = m_dir;
    QSignalSpy finished(w.get(), &MainWindow::chooserFinished);
    QSignalSpy loaded(w->m_pane->model, &DirectoryModel::loaded);
    w->startChooser(request);
    QTRY_VERIFY_WITH_TIMEOUT(!loaded.isEmpty(), 10000);
    QCOMPARE(w->m_pane->current, m_dir);

    // Nothing chosen: no answer yet.
    w->m_chooser->accept();
    QCOMPARE(finished.count(), 0);

    select({a, b});
    w->openSelection(); // Enter / double-click
    QCOMPARE(finished.count(), 1);
    QCOMPARE(finished.first().at(0).toUInt(), uint(FileChooserPortal::Success));
    QStringList uris = finished.first().at(1).toMap().value("uris").toStringList();
    uris.sort();
    QCOMPARE(uris, (QStringList{QUrl::fromLocalFile(a).toString(), QUrl::fromLocalFile(b).toString()}));
    QVERIFY(exists(a) && exists(b));

    // Answered once: more input changes nothing.
    w->m_chooser->accept();
    w->cancelChooser();
    QCOMPARE(finished.count(), 1);
}

// Save onto an existing file: the app will overwrite it, so ask first.
void TestMainWindow::chooserSaveAsksBeforeReplacing()
{
    const QString existing = make("report.txt", "old");
    ChooserRequest request;
    request.mode = ChooserRequest::Mode::Save;
    request.appId = "org.example.Test";
    request.currentFolder = m_dir;
    request.currentName = "report.txt";
    QSignalSpy finished(w.get(), &MainWindow::chooserFinished);
    QSignalSpy loaded(w->m_pane->model, &DirectoryModel::loaded);
    w->startChooser(request);
    QTRY_VERIFY_WITH_TIMEOUT(!loaded.isEmpty(), 10000);
    QCOMPARE(w->m_chooser->bar()->name(), QStringLiteral("report.txt"));

    answerNext("Cancel");
    w->m_chooser->accept();
    QCOMPARE(m_asked, 1);
    QVERIFY(m_question.contains("report.txt"));
    QCOMPARE(finished.count(), 0); // still open, nothing answered

    // A name that is free needs no question.
    w->m_chooser->bar()->setName("new.txt");
    w->m_chooser->accept();
    QCOMPARE(m_asked, 1);
    QCOMPARE(finished.count(), 1);
    QCOMPARE(finished.first().at(1).toMap().value("uris").toStringList(),
             QStringList{QUrl::fromLocalFile(m_dir + "/new.txt").toString()});
    // The dialog itself never writes: the app does.
    QVERIFY(!exists(m_dir + "/new.txt"));
    QFile f(existing);
    QVERIFY(f.open(QIODevice::ReadOnly));
    QCOMPARE(f.readAll(), QByteArray("old"));
}

void TestMainWindow::chooserCancelAnswersOnce()
{
    ChooserRequest request;
    request.mode = ChooserRequest::Mode::Save;
    request.currentFolder = m_dir;
    request.currentName = "x.txt";
    QSignalSpy finished(w.get(), &MainWindow::chooserFinished);
    w->startChooser(request);

    w->cancelChooser();
    QCOMPARE(finished.count(), 1);
    QCOMPARE(finished.first().at(0).toUInt(), uint(FileChooserPortal::Cancelled));
    w->m_chooser->accept();
    w->cancelChooser();
    QCOMPARE(finished.count(), 1);
}

#endif

int main(int argc, char **argv)
{
    TestSupport::Sandbox sandbox; // before QApplication
    if (!sandbox.isValid())
        return 1;
    const QString root = sandbox.path();
    qputenv("QT_QPA_PLATFORM", "offscreen"); // always: these tests show windows
    // Never eject a real drive, launch a real app or change default apps.
    const QString fake = root + "/fake-program";
    {
        QFile f(fake);
        if (!f.open(QIODevice::WriteOnly))
            return 1;
        f.write("#!/bin/sh\necho \"$0 $*\" >> \"$(dirname \"$0\")/fake-program.log\"\nexit 0\n");
    }
    QFile::setPermissions(fake, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    for (const char *name : {"ARGONAUT_EJECT_PROGRAM", "ARGONAUT_GIO_PROGRAM", "ARGONAUT_EMAIL_PROGRAM",
                             "ARGONAUT_GSETTINGS_PROGRAM", "ARGONAUT_TRACKER_PROGRAM", "ARGONAUT_SYSTEMCTL_PROGRAM"})
        qputenv(name, QFile::encodeName(fake));
    qputenv("ARGONAUT_NATIVE_DECORATIONS", "1");
    // Only the test's own extension (switched off until a test wants it).
    QDir().mkpath(root + "/extensions/fake");
    {
        QFile manifest(root + "/extensions/fake/extension.json");
        if (!manifest.open(QIODevice::WriteOnly))
            return 1;
        manifest.write(R"({"id": "fake", "name": "Fake", "protocol": 1, "addresses": ["fake"], "exec": ["fake.py"]})");
    }
    QFile::copy(QStringLiteral(FAKE_EXTENSION), root + "/extensions/fake/fake.py");
    QFile::setPermissions(root + "/extensions/fake/fake.py", QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    qputenv("ARGONAUT_EXTENSIONS_PATH", QFile::encodeName(root + "/extensions"));

    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName("ArgonautTest");
    QCoreApplication::setApplicationName("ArgonautTest");
    TestMainWindow test(root);
    return QTest::qExec(&test, argc, argv);
}

// Space: the preview is a pane inside the window, at the right of the files.
// It follows the selected file, stays through a folder change (empty until
// something is selected), and Space again closes it.
void TestMainWindow::previewIsAPaneThatFollowsTheSelection()
{
    const QString a = make("notes.txt"), b = make("other.txt");
    QDir(m_dir).mkdir("sub");
    open(m_dir);
    auto current = [&](const QString &path) {
        w->m_pane->view->setCurrentIndex(w->toView(w->m_pane->model->indexForPath(path)));
    };
    current(a);
    QTest::keyClick(w->m_pane->view, Qt::Key_Space);
    QVERIFY(w->quickLookVisible());
    QuickLook *preview = w->m_quickLook;
    QVERIFY(!preview->isWindow()); // inside the main window,
    QCOMPARE(preview->parentWidget(), w->m_previewSplitter);
    QVERIFY(preview->mapTo(w.get(), QPoint(0, 0)).x() // at the right of the files
            > w->m_pane->view->mapTo(w.get(), QPoint(0, 0)).x());
    QCOMPARE(preview->path(), a);
    QTRY_COMPARE(preview->kind(), Preview::Kind::Text);

    current(b); // it follows the selection
    QCOMPARE(preview->path(), b);

    open(m_dir + "/sub"); // and stays, empty, in another folder
    QVERIFY(w->quickLookVisible());
    QVERIFY(preview->path().isEmpty());

    QTest::keyClick(w->m_pane->view, Qt::Key_Space);
    QVERIFY(!w->quickLookVisible());
    QVERIFY(exists(a) && exists(b)); // looking changes nothing
}

// A command from an extension in the right-click menu: one that wants a
// question asked first isn't sent to the extension when the answer is Cancel.
void TestMainWindow::extensionCommandsAskFirst()
{
    if (QStandardPaths::findExecutable("python3").isEmpty())
        QSKIP("python3 isn't installed (the test extension is a Python script)");
    Extensions::Host *host = Extensions::Host::instance();
    host->setEnabled("fake", true);
    const QString a = make("a.txt");
    open(m_dir);
    select({a});
    auto risky = [&](QMenu &menu) -> QAction * {
        w->addExtensionCommands(&menu, {a});
        for (QAction *action : menu.actions()) {
            if (action->text() == QStringLiteral("Risky…"))
                return action;
        }
        return nullptr;
    };
    QMenu menu;
    QTRY_VERIFY_WITH_TIMEOUT([&] { menu.clear(); return risky(menu) != nullptr; }(), 10000);
    QSignalSpy said(host, &Extensions::Host::message);

    answerNext("Cancel");
    risky(menu)->trigger();
    QCOMPARE(m_asked, 1);
    QVERIFY(m_question.contains("Really do the risky thing?"));
    QTest::qWait(500);
    QVERIFY(said.isEmpty()); // the extension never heard of it

    answerNext("Risky");
    menu.clear();
    risky(menu)->trigger();
    QCOMPARE(m_asked, 2);
    QTRY_VERIFY_WITH_TIMEOUT(!said.isEmpty(), 10000);
    QCOMPARE(said.first().at(1).toString(), QStringLiteral("invoked ask on 1: a.txt"));
    // What it says is shown in the window.
    QTRY_COMPARE(w->m_pane->floatingBar->text(), QStringLiteral("invoked ask on 1: a.txt"));

    host->setEnabled("fake", false);
    QMenu after;
    QVERIFY(!risky(after));
}

// Edit → Extensions: a window of its own that lists what was found, with a
// switch that really switches (an added extension starts off).
void TestMainWindow::extensionsWindowListsAndSwitches()
{
    if (QStandardPaths::findExecutable("python3").isEmpty())
        QSKIP("python3 isn't installed (the test extension is a Python script)");
    Extensions::Host *host = Extensions::Host::instance();
    host->setEnabled("fake", false);
    open(m_dir);
    w->m_extensionsAct->trigger();
    auto *dialog = w->findChild<ExtensionsDialog *>();
    QVERIFY(dialog);
    QVERIFY(dialog->isVisible());
    QVERIFY(dialog->isWindow());
    QLabel *state = nullptr;
    QTRY_VERIFY((state = dialog->findChild<QLabel *>("extState")) != nullptr);
    QCOMPARE(state->property("extensionId").toString(), QStringLiteral("fake"));
    QCOMPARE(state->text(), QStringLiteral("Off"));
    auto *toggle = dialog->findChild<QCheckBox *>("prefCheck");
    QVERIFY(toggle && !toggle->isChecked());

    toggle->click();
    QVERIFY(host->isEnabled("fake"));
    QTRY_VERIFY_WITH_TIMEOUT(host->isRunning("fake"), 10000);
    // The list was rebuilt; its new label says so.
    QTRY_COMPARE_WITH_TIMEOUT(dialog->findChild<QLabel *>("extState")->text(), QStringLiteral("Running"), 5000);
    dialog->findChild<QCheckBox *>("prefCheck")->click();
    QVERIFY(!host->isEnabled("fake"));
    QVERIFY(!host->isRunning("fake"));
    dialog->close();
}

// The cross-fade on a change of folder: it runs over the new folder (which
// is the current one at once), a second change during it ends it instead of
// queueing another, it ends by itself, and the switch turns it off.
void TestMainWindow::fadeNeverGetsInTheWay()
{
    make("a/one.txt");
    make("b/two.txt");
    open(m_dir);
    FadeOverlay *fade = w->m_pane->fade;
    QVERIFY(fade);
    QTRY_VERIFY(!fade->isRunning());

    QVERIFY(w->navigateTo(m_dir + "/a"));
    QVERIFY(fade->isRunning());
    QVERIFY(fade->isVisible());
    QCOMPARE(w->m_pane->current, m_dir + "/a"); // nothing waits for the effect
    QVERIFY(fade->testAttribute(Qt::WA_TransparentForMouseEvents));

    QVERIFY(w->navigateTo(m_dir + "/b")); // quickly again: out of the way
    QVERIFY(!fade->isRunning());
    QVERIFY(!fade->isVisible());
    QCOMPARE(w->m_pane->current, m_dir + "/b");
    QTRY_VERIFY(w->m_pane->model->indexForPath(m_dir + "/b/two.txt").isValid());

    QVERIFY(w->navigateTo(m_dir));
    QVERIFY(fade->isRunning());
    QTRY_VERIFY_WITH_TIMEOUT(!fade->isRunning(), 2000); // ends by itself
    QVERIFY(!fade->isVisible());

    FadeOverlay::setEnabled(false);
    QVERIFY(w->navigateTo(m_dir + "/a"));
    QVERIFY(!fade->isRunning());
    FadeOverlay::setEnabled(true);
}

// Space on a file an extension previews: the pane shows the extension's
// answer; "not from me" falls back to the built-in preview; rich text loads
// nothing it names; and a program in disguise is never handed to it.
void TestMainWindow::previewFromAnExtension()
{
    if (QStandardPaths::findExecutable("python3").isEmpty())
        QSKIP("python3 isn't installed (the test extension is a Python script)");
    Extensions::Host *host = Extensions::Host::instance();
    host->setEnabled("fake", true);
    const QString table = make("table.csv", "a,b\n1,2\n"), nothing = make("nothing.csv", "a,b\n"),
                  rich = make("rich.csv", "a,b\n"), picture = make("picture.csv", "a,b\n");
    const QString disguised = make("program.csv", QByteArray("\x7f" "ELF\x02\x01\x01\0\0\0\0\0\0\0\0\0\x02\0\x3e\0", 20)
                                                      + QByteArray(200, '\0'));
    open(m_dir);
    QTRY_VERIFY_WITH_TIMEOUT(host->canPreview("text/csv"), 10000);
    auto current = [&](const QString &path) {
        w->m_pane->view->setCurrentIndex(w->toView(w->m_pane->model->indexForPath(path)));
    };
    current(table);
    QTest::keyClick(w->m_pane->view, Qt::Key_Space);
    QVERIFY(w->quickLookVisible());
    QuickLook *preview = w->m_quickLook;
    QTRY_VERIFY_WITH_TIMEOUT(preview->fromExtension(), 10000);
    QCOMPARE(preview->shownText(), QStringLiteral("FAKE PREVIEW of table.csv as text/csv"));

    current(nothing); // the extension passes: our own text preview
    QTRY_COMPARE_WITH_TIMEOUT(preview->shownText(), QStringLiteral("a,b\n"), 10000);
    QVERIFY(!preview->fromExtension());

    current(rich);
    QTRY_VERIFY_WITH_TIMEOUT(preview->fromExtension() && preview->shownText().contains("Title"), 10000);
    QVERIFY(preview->shownText().contains("Body"));

    current(picture);
    QTRY_VERIFY_WITH_TIMEOUT(preview->fromExtension() && preview->kind() == Preview::Kind::Image, 10000);

    // A program named like a table: blocked before anyone is asked.
    QTRY_COMPARE_WITH_TIMEOUT(w->m_pane->model->indexForPath(disguised).data(DirectoryModel::VerdictRole).toInt(),
                              int(ContentType::Verdict::Dangerous), 10000);
    current(disguised);
    QTRY_COMPARE_WITH_TIMEOUT(preview->kind(), Preview::Kind::Blocked, 10000);
    QVERIFY(!preview->fromExtension());
    host->setEnabled("fake", false);
}

// Markdown in the preview pane: Qt formats it, RichTextStyle gives it the
// look of a page, and never touches the text itself.
void TestMainWindow::richTextGetsAPageLook()
{
    QTextDocument doc;
    doc.setMarkdown("# Title\n\nText with `code` and a [link](https://example.org).\n\n"
                    "> quoted\n\n```\nline one\nline two\n```\n\n| A | B |\n|---|---|\n| 1 | 2 |\n");
    const QString before = doc.toPlainText();
    const Theme theme = currentTheme();
    RichTextStyle::apply(&doc, theme);
    QCOMPARE(doc.toPlainText(), before);

    auto blockWith = [&](const QString &text) {
        for (QTextBlock b = doc.begin(); b.isValid(); b = b.next()) {
            if (b.text().contains(text))
                return b;
        }
        return QTextBlock();
    };
    auto formatOf = [&](const QString &text) {
        const QTextBlock b = blockWith(text);
        for (auto it = b.begin(); !it.atEnd(); ++it) {
            if (it.fragment().text().contains(text))
                return it.fragment().charFormat();
        }
        return QTextCharFormat();
    };
    const qreal body = doc.defaultFont().pointSizeF() > 0 ? doc.defaultFont().pointSizeF() : 10;
    // A heading is clearly bigger than the text, and bold.
    QVERIFY(formatOf("Title").fontPointSize() > body * 1.5);
    QCOMPARE(formatOf("Title").fontWeight(), int(QFont::Bold));
    QVERIFY(!formatOf("Title").hasProperty(QTextFormat::FontSizeAdjustment));
    // A link has the theme's colour; code in a line is tinted and fixed-width.
    QCOMPARE(formatOf("link").foreground().color(), theme.accent);
    QVERIFY(formatOf("code").fontFixedPitch());
    QVERIFY(formatOf("code").background().style() != Qt::NoBrush);
    // A code block is one tinted box that wraps.
    const QTextBlock one = blockWith("line one"), two = blockWith("line two");
    QVERIFY(one.blockFormat().background().style() != Qt::NoBrush);
    QVERIFY(!one.blockFormat().nonBreakableLines());
    QCOMPARE(one.blockFormat().bottomMargin(), 0.0);
    QCOMPARE(two.blockFormat().topMargin(), 0.0);
    // A quote is set in and lighter; ordinary text has room between lines.
    QVERIFY(blockWith("quoted").blockFormat().leftMargin() > 0);
    QVERIFY(formatOf("quoted").foreground().color() != theme.fg);
    QCOMPARE(blockWith("Text with").blockFormat().lineHeight(), 130.0);
    // A table has lines and a bold header row.
    QTextTable *table = nullptr;
    for (auto it = doc.rootFrame()->begin(); !it.atEnd(); ++it) {
        if (auto *t = qobject_cast<QTextTable *>(it.currentFrame()))
            table = t;
    }
    QVERIFY(table);
    QCOMPARE(table->format().border(), 1.0);
    QVERIFY(table->format().borderCollapse());
    QCOMPARE(table->cellAt(0, 0).firstCursorPosition().block().begin().fragment().charFormat().fontWeight(), int(QFont::Bold));
    QVERIFY(table->cellAt(1, 0).firstCursorPosition().block().begin().fragment().charFormat().fontWeight() != int(QFont::Bold));
}

// Ctrl+Up in the terminal: the files go to the folder the shell is in.
void TestMainWindow::terminalBringsTheFilesAlong()
{
#if defined(ARGONAUT_TERMINAL) && defined(Q_OS_LINUX)
    const QString other = m_dir + "/elsewhere";
    QDir().mkpath(other);
    make("elsewhere/inside.txt");
    open(m_dir);
    w->m_terminalAct->trigger();
    TerminalWidget *terminal = w->m_terminal;
    QVERIFY(terminal && terminal->isRunning());
    QTRY_VERIFY_WITH_TIMEOUT(terminal->isShellIdle() && terminal->shellDirectory() == m_dir, 10000);

    // `cd` in the shell: the files stay where they are...
    QTRY_VERIFY_WITH_TIMEOUT(terminal->changeDirectory(other), 10000);
    QTRY_COMPARE_WITH_TIMEOUT(terminal->shellDirectory(), other, 10000);
    QCOMPARE(w->m_pane->current, m_dir);
    // ...until Ctrl+Up.
    QTest::keyClick(terminal, Qt::Key_Up, Qt::ControlModifier);
    QCOMPARE(w->m_pane->current, other);
    QTRY_VERIFY(w->m_pane->model->indexForPath(other + "/inside.txt").isValid());
    QCOMPARE(terminal->shellDirectory(), other); // nothing was typed into the shell
    // Already there: the folder is read again, without a new step in the history.
    QVERIFY(w->m_backAct->isEnabled());
    const int history = int(w->m_pane->back.size());
    QTest::keyClick(terminal, Qt::Key_Up, Qt::ControlModifier);
    QCOMPARE(int(w->m_pane->back.size()), history);
    w->m_terminalAct->trigger();
#else
    QSKIP("no embedded terminal in this build");
#endif
}

// Themes are files: one in a themes folder is offered beside the built-in
// ones (Preferences, View → Theme, the command palette), its colours are
// the window's, and saving the file again changes the window at once.
void TestMainWindow::themesFromFiles()
{
    const QString folder = m_dir + "/themes-folder";
    QDir().mkpath(folder);
    qputenv("ARGONAUT_THEMES_PATH", QFile::encodeName(folder));
    struct Restore
    {
        ~Restore()
        {
            setThemeKey("system");
            qputenv("ARGONAUT_THEMES_PATH", ""); // as the tests have it: no theme files
        }
    } restore;
    auto write = [&](const QString &name, const QByteArray &json) {
        QFile file(folder + "/" + name);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(json);
    };
    write("Deep Sea.json", R"({"name": "Deep & Sea", "window": "#0b1d2a", "sidebar": "#102838", "text": "#e0f0ff", "accent": "#00c2a8"})");
    write("Paper.json", R"({"window": "#fffdf5", "text": "#222222", "accent": "#aa3300", "border": "#11223344"})");
    write("Broken.json", R"({"window": "#000000", "text": "not a colour"})");
    write("Nonsense.json", "this is not JSON");
    write("notes.txt", "not a theme");

    // Reading: three colours are enough; dark or light is told from the
    // background; any other colour can be said outright.
    ThemeFile read = readThemeFile(folder + "/Deep Sea.json");
    QVERIFY2(read.problem.isEmpty(), qPrintable(read.problem));
    QCOMPARE(read.name, QStringLiteral("Deep & Sea"));
    QVERIFY(read.theme.dark);
    QCOMPARE(read.theme.window, QColor("#0b1d2a"));
    QCOMPARE(read.theme.accent, QColor("#00c2a8"));
    read = readThemeFile(folder + "/Paper.json");
    QVERIFY(read.problem.isEmpty());
    QCOMPARE(read.name, QStringLiteral("Paper")); // no name inside: the file's
    QVERIFY(!read.theme.dark);
    QVERIFY(read.theme.sidebar.isValid() && read.theme.sidebar != read.theme.window); // worked out
    QCOMPARE(read.theme.border, QColor("#11223344"));
    QVERIFY(!readThemeFile(folder + "/Broken.json").problem.isEmpty());
    QVERIFY(!readThemeFile(folder + "/Nonsense.json").problem.isEmpty());
    QVERIFY(!readThemeFile(folder + "/missing.json").problem.isEmpty());

    // Offered after the built-in ones; the broken ones aren't.
    QStringList keys, names;
    for (const ThemeChoice &theme : allThemes()) {
        keys << theme.key;
        names << theme.name;
    }
    QCOMPARE(keys.mid(0, 3), (QStringList{"system", "light", "dark"}));
    QVERIFY(keys.contains("forest"));
    QCOMPARE(keys.mid(keys.size() - 2), (QStringList{"file:Deep Sea", "file:Paper"}));
    QVERIFY(!names.contains("Broken") && !names.contains("Nonsense") && !names.contains("notes"));

    // Chosen: the window is dressed in its colours, and it is remembered.
    open(m_dir);
    setThemeKey("file:Deep Sea");
    QCOMPARE(currentThemeKey(), QStringLiteral("file:Deep Sea"));
    QCOMPARE(savedThemeKey(), QStringLiteral("file:Deep Sea"));
    QCOMPARE(currentTheme().window, QColor("#0b1d2a"));
    QTRY_COMPARE(w->m_theme.window, QColor("#0b1d2a"));
    QCOMPARE(QApplication::palette().color(QPalette::Window), QColor("#0b1d2a"));
    QVERIFY(w->styleSheet().contains("11, 29, 42") || w->styleSheet().contains("#0b1d2a"));

    // The file saved again with another colour: the window follows.
    write("Deep Sea.json", R"({"name": "Deep & Sea", "window": "#0b1d2a", "sidebar": "#102838", "text": "#e0f0ff", "accent": "#ff5500"})");
    QTRY_COMPARE_WITH_TIMEOUT(currentTheme().accent, QColor("#ff5500"), 10000);
    QTRY_COMPARE(w->m_theme.accent, QColor("#ff5500"));
    // Broken halfway through an edit: the last good colours stay.
    write("Deep Sea.json", "{\"window\": ");
    QTest::qWait(500);
    QCOMPARE(currentTheme().accent, QColor("#ff5500"));
    QCOMPARE(currentThemeKey(), QStringLiteral("file:Deep Sea"));
    write("Deep Sea.json", R"({"name": "Deep & Sea", "window": "#0b1d2a", "text": "#e0f0ff", "accent": "#00c2a8"})");
    QTRY_COMPARE_WITH_TIMEOUT(currentTheme().accent, QColor("#00c2a8"), 10000);

    // View → Theme lists them all, the one in use ticked; choosing one applies it.
    w->fillThemeMenu();
    QAction *deepSea = nullptr, *dark = nullptr;
    for (QAction *action : w->m_themeMenu->actions()) {
        if (action->text() == "Deep && Sea") // a name is a name
            deepSea = action;
        if (action->text() == "Dark")
            dark = action;
    }
    QVERIFY(deepSea && dark);
    QVERIFY(deepSea->isChecked() && !dark->isChecked());
    dark->trigger();
    QCOMPARE(currentThemeKey(), QStringLiteral("dark"));
    QTRY_VERIFY(w->m_theme.dark && w->m_theme.window != QColor("#0b1d2a"));

    // The command palette has them: "View › Theme › Paper".
    w->showCommandPalette();
    QAction *paper = nullptr;
    for (const CommandPalette::Command &command : w->m_palette->commands()) {
        if (command.name == "Paper" && command.where.contains("Theme"))
            paper = command.action;
    }
    w->m_palette->close();
    QVERIFY(paper);
    paper->trigger();
    QCOMPARE(currentThemeKey(), QStringLiteral("file:Paper"));
    QTRY_COMPARE(w->m_theme.window, QColor("#fffdf5"));

    // A theme whose file is gone: Follow System, not a broken window.
    QFile::remove(folder + "/Paper.json");
    applyThemeKey("file:Paper");
    QCOMPARE(currentThemeKey(), QStringLiteral("system"));
    applyThemeKey("file:../../etc/passwd"); // a key is a plain name
    QCOMPARE(currentThemeKey(), QStringLiteral("system"));

    // "Save This Theme as a File": every colour written out, read back the same; never over another file.
    const Theme forest = themeForKey("forest");
    QString error;
    const QString saved = saveThemeFile(forest, "My / Forest", &error);
    QVERIFY2(!saved.isEmpty(), qPrintable(error));
    QVERIFY(saved.startsWith(userThemesFolder() + "/"));
    QVERIFY(!QFileInfo(saved).fileName().contains('/'));
    read = readThemeFile(saved);
    QVERIFY2(read.problem.isEmpty(), qPrintable(read.problem));
    QCOMPARE(read.theme.window, forest.window);
    QCOMPARE(read.theme.sidebar, forest.sidebar);
    QCOMPARE(read.theme.fg, forest.fg);
    QCOMPARE(read.theme.accent, forest.accent);
    QCOMPARE(read.theme.itemSelected, forest.itemSelected); // translucent ones keep their alpha
    QCOMPARE(read.theme.border, forest.border);
    QCOMPARE(read.theme.dark, forest.dark);
    const QString again = saveThemeFile(forest, "My / Forest", &error);
    QVERIFY(!again.isEmpty() && again != saved);
    QFile::remove(saved);
    QFile::remove(again);
}

// What an extension asks the user while it works is a dialog of ours that
// names the extension; what is filled in goes back to it, Cancel too. A
// kind of address an extension said is its goes to it when typed; a server
// address nobody answers for says so.
void TestMainWindow::extensionFormsAndAddresses()
{
    if (QStandardPaths::findExecutable("python3").isEmpty())
        QSKIP("python3 isn't installed (the test extension is a Python script)");
    Extensions::Host *host = Extensions::Host::instance();
    const QString fakeFolder = QString::fromLocal8Bit(qgetenv("ARGONAUT_EXTENSIONS_PATH")) + "/fake";
    QFile::remove(fakeFolder + "/forms");
    auto logged = [&] {
        QFile file(fakeFolder + "/forms");
        return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()) : QString();
    };
    open(m_dir);

    // Nobody answers for server addresses: said, and nothing opens.
    QVERIFY(!w->navigateTo("sftp://example.com/"));
    QVERIFY(w->m_pane->message.contains("switched off"));
    QCOMPARE(w->m_pane->current, m_dir);

    host->setEnabled("fake", true);
    auto command = [&](const QString &id) {
        for (const Extensions::Command &c : host->commands(m_dir, {})) {
            if (c.id == id) {
                host->invoke(c, m_dir, {});
                return true;
            }
        }
        return false;
    };
    // The form it sent when it started, unasked, was refused and never shown.
    QTRY_VERIFY_WITH_TIMEOUT(logged().contains("\"sneaky\""), 10000);
    QVERIFY(logged().contains("\"cancelled\": true"));
    QVERIFY(!QApplication::activeModalWidget());

    // Looks at the next dialog, fills it in and presses a button (or Cancel).
    // One button for each dialog to come, in order (a dialog opens inside
    // the event loop the test is waiting in, so its answer must be ready).
    QStringList seenAll;
    QString seen;
    auto answerForms = [&](const QStringList &buttons, bool tickAnonymous = false) {
        auto *timer = new QTimer(this);
        timer->setInterval(10);
        auto left = std::make_shared<QStringList>(buttons);
        auto last = std::make_shared<QPointer<QDialog>>();
        connect(timer, &QTimer::timeout, this, [&seen, &seenAll, timer, left, last, tickAnonymous] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            if (!dialog || dialog == last->data())
                return;
            *last = dialog;
            const QString button = left->takeFirst();
            if (left->isEmpty()) {
                timer->stop();
                timer->deleteLater();
            }
            QStringList parts{dialog->windowTitle()};
            for (QLabel *label : dialog->findChildren<QLabel *>())
                parts << label->text();
            for (QLineEdit *edit : dialog->findChildren<QLineEdit *>()) {
                if (edit->echoMode() == QLineEdit::Password) {
                    parts << "secret=" + edit->text();
                    edit->setText("s3cret");
                } else if (!qobject_cast<QComboBox *>(edit->parentWidget())) {
                    parts << "text=" + edit->text();
                    edit->setText("alice");
                }
            }
            for (QComboBox *combo : dialog->findChildren<QComboBox *>()) {
                parts << "combo=" + combo->currentText() + (combo->isEditable() ? "/editable" : "");
                if (combo->isEditable())
                    combo->setEditText("typed");
            }
            for (QCheckBox *box : dialog->findChildren<QCheckBox *>()) {
                parts << "check=" + box->text();
                if (tickAnonymous)
                    box->setChecked(true);
            }
            for (QLineEdit *edit : dialog->findChildren<QLineEdit *>()) {
                if (edit->echoMode() == QLineEdit::Password)
                    parts << QString("enabled=%1").arg(edit->isEnabled());
            }
            seen = parts.join("|");
            seenAll << seen;
            for (QAbstractButton *b : dialog->findChildren<QAbstractButton *>()) {
                if (b->text() == button) {
                    b->click();
                    return;
                }
            }
            dialog->reject();
        });
        timer->start();
    };

    // A login: its texts as plain text, the fields that make sense, who asks.
    QSignalSpy said(host, &Extensions::Host::message);
    answerForms({"Connect", "No && Never"}); // the login, then its follow-up question
    QVERIFY(command("form"));
    QTRY_COMPARE_WITH_TIMEOUT(int(seenAll.size()), 2, 10000);
    seen = seenAll.first();
    QVERIFY2(seen.startsWith("Log In|Log in to nas|Enter <b>it</b>|That didn't work|"), qPrintable(seen)); // one line; not obeyed as HTML
    QVERIFY2(seen.contains("Asked by the extension “Fake”."), qPrintable(seen));
    QVERIFY2(seen.contains("text=me") && seen.contains("secret=|"), qPrintable(seen)); // a password never starts filled in
    QVERIFY2(seen.contains("combo=music|") && seen.contains("combo=/editable"), qPrintable(seen));
    QVERIFY(seen.contains("check=Anonymously") && seen.contains("enabled=1"));
    QVERIFY(!seen.contains("Twice") && !seen.contains("Nothing to choose"));
    // What was filled in went to the extension, and to nobody's log of ours.
    auto wasSaid = [&said](const QString &text) {
        for (const auto &entry : said) {
            if (entry.at(1).toString() == text)
                return true;
        }
        return false;
    };
    QTRY_VERIFY_WITH_TIMEOUT(wasSaid("form alice|s3cret|false|music|typed button 0"), 10000);
    // The second form it sent while the first was open was refused.
    QVERIFY(logged().contains("\"second\""));

    // Its follow-up question: buttons only, a name is a name; Cancel is told too.
    seen = seenAll.last();
    QVERIFY2(seen.contains("Are you sure?"), qPrintable(seen));
    QTRY_VERIFY_WITH_TIMEOUT(logged().contains("\"again\""), 10000);
    QVERIFY(logged().contains("\"button\": 1, \"id\": \"again\""));

    // Ticking the box that stands alone turns the other fields off; Cancel.
    seenAll.clear();
    said.clear();
    answerForms({"Cancel"}, true);
    QVERIFY(command("form"));
    QTRY_COMPARE_WITH_TIMEOUT(int(seenAll.size()), 1, 10000);
    QVERIFY(seenAll.first().contains("enabled=0"));
    QTRY_VERIFY_WITH_TIMEOUT(logged().contains("{\"cancelled\": true, \"id\": \"login\""), 10000);

    // An address of a kind it said is its: it is told, and opens a folder.
    said.clear();
    QCOMPARE(w->m_pane->current, m_dir);
    QVERIFY(host->handlesAddress("fake://somewhere/else"));
    QVERIFY(!w->navigateTo("fake://somewhere/else"));
    QTRY_COMPARE_WITH_TIMEOUT(w->m_pane->current, fakeFolder, 10000);
    bool told = false;
    for (const auto &entry : said)
        told = told || entry.at(1).toString() == "address fake://somewhere/else";
    QVERIFY(told);
    host->setEnabled("fake", false);
}

// An extension's rows in the sidebar: a click on one that is a folder opens
// it; one that isn't asks its extension, which answers with the folder to
// open; ⏏ is the extension's to carry out.
void TestMainWindow::extensionPlacesInTheSidebar()
{
    if (QStandardPaths::findExecutable("python3").isEmpty())
        QSKIP("python3 isn't installed (the test extension is a Python script)");
    Extensions::Host *host = Extensions::Host::instance();
    const QString fakeFolder = QString::fromLocal8Bit(qgetenv("ARGONAUT_EXTENSIONS_PATH")) + "/fake";
    auto row = [this](const QString &text) -> QListWidgetItem * {
        for (int i = 0; i < w->m_sidebar->count(); ++i) {
            if (w->m_sidebar->item(i)->text() == text)
                return w->m_sidebar->item(i);
        }
        return nullptr;
    };
    open(m_dir);
    QVERIFY(!row("Fake Cloud"));
    host->setEnabled("fake", true);
    QTRY_VERIFY_WITH_TIMEOUT(row("Fake Cloud") && row("Fake Folder"), 10000);
    QCOMPARE(Sidebar::kindOf(row("Fake Cloud")), Sidebar::ExtensionItem);
    QVERIFY(Sidebar::canEject(row("Fake Cloud")));
    QVERIFY(!Sidebar::canEject(row("Fake Folder")));
    QVERIFY(row("Fake Cloud")->toolTip().contains("A pretend drive") && row("Fake Cloud")->toolTip().contains("Fake"));

    // A row with a folder is a place like any other.
    QCOMPARE(Sidebar::pathOf(row("Fake Folder")), fakeFolder);
    emit w->m_sidebar->itemClicked(row("Fake Folder"));
    QCOMPARE(w->m_pane->current, fakeFolder);
    QCOMPARE(w->m_sidebar->currentItem(), row("Fake Folder"));

    // A row without one: nothing opens until its extension says what.
    open(m_dir);
    emit w->m_sidebar->itemClicked(row("Fake Cloud"));
    QCOMPARE(w->m_pane->current, m_dir);
    QTRY_COMPARE_WITH_TIMEOUT(w->m_pane->current, fakeFolder, 10000);
    QTest::qWait(200);
    QCOMPARE(w->m_pane->current, fakeFolder); // not the second folder it named
    // No tab for it on a middle click, and nothing can be dropped on it.
    const int tabs = w->m_tabBar->count();
    QListWidgetItem *cloud = row("Fake Cloud");
    const QPoint middle = w->m_sidebar->visualItemRect(cloud).center();
    QTest::mouseClick(w->m_sidebar->viewport(), Qt::MiddleButton, Qt::NoModifier, middle);
    QCOMPARE(w->m_tabBar->count(), tabs);

    // ⏏ goes to the extension, which takes the row away; the drive code isn't asked.
    QPoint eject;
    const QRect rect = w->m_sidebar->visualItemRect(cloud);
    for (int x = rect.right(); x > rect.left() && eject.isNull(); --x) {
        if (w->m_sidebar->ejectButtonHit(cloud, QPoint(x, rect.center().y())))
            eject = QPoint(x - 2, rect.center().y());
    }
    QVERIFY(!eject.isNull());
    QTest::mouseClick(w->m_sidebar->viewport(), Qt::LeftButton, Qt::NoModifier, eject);
    QTRY_VERIFY_WITH_TIMEOUT(!row("Fake Cloud"), 10000);
    QVERIFY(row("Fake Folder"));

    // The extension switched off: its rows are gone.
    host->setEnabled("fake", false);
    QTRY_VERIFY_WITH_TIMEOUT(!row("Fake Folder"), 10000);
}

// A result with a name (a dashboard): its extension is told it is on screen
// and how much room there is, may replace it while it is, and is told when
// it no longer is. A late update never takes the pane from a file or opens it.
// A picture from an extension that is longer than the pane (a list it
// drew: "scroll") is shown at its own size and scrolls; made again while it
// is shown, it stays where it was scrolled to. Without "scroll" a picture
// is scaled to fit, as before.
void TestMainWindow::longPictureFromAnExtensionScrolls()
{
    open(m_dir);
    QImage tall(qEnvironmentVariableIsSet("ARGONAUT_TEST_PICTURE") ? QImage(qEnvironmentVariable("ARGONAUT_TEST_PICTURE")) : QImage(200, 3000, QImage::Format_RGB32));
    if (!qEnvironmentVariableIsSet("ARGONAUT_TEST_PICTURE"))
    tall.fill(QColor("#336699"));
    const QString file = m_dir + "/tall.png";
    QVERIFY(tall.save(file));
    Extensions::PreviewReply content;
    content.kind = Extensions::PreviewReply::Image;
    content.image = file;
    content.extension = "fake";
    content.resultId = "long";
    content.scroll = true;
    w->showExtensionResult("fake", "Long", content);
    QVERIFY(w->m_quickLook && w->m_quickLook->isVisible());
    QTRY_VERIFY(w->m_quickLook->pictureScrollRange() > 1500); // 3000 pixels of it, a pane's height on screen
    QCOMPARE(w->m_quickLook->pictureScrollValue(), 0);
    w->m_quickLook->scrollPictureTo(1200);
    QCOMPARE(w->m_quickLook->pictureScrollValue(), 1200);
    if (qEnvironmentVariableIsSet("ARGONAUT_TEST_GRAB")) { // to look at it
        QTest::qWait(200);
        w->grab().save(qEnvironmentVariable("ARGONAUT_TEST_GRAB"));
    }
    // The same result again: where it was.
    content.update = true;
    w->showExtensionResult("fake", "Long", content);
    QCOMPARE(w->m_quickLook->pictureScrollValue(), 1200);
    // Not asked to scroll: the whole picture, made to fit.
    content.update = false;
    content.resultId = "fits";
    content.scroll = false;
    w->showExtensionResult("fake", "Fits", content);
    QCOMPARE(w->m_quickLook->pictureScrollRange(), -1);
}

void TestMainWindow::extensionResultStaysLive()
{
    if (QStandardPaths::findExecutable("python3").isEmpty())
        QSKIP("python3 isn't installed (the test extension is a Python script)");
    Extensions::Host *host = Extensions::Host::instance();
    host->setEnabled("fake", true);
    const QString a = make("a.txt");
    open(m_dir);
    const QString log = QString::fromLocal8Bit(qgetenv("ARGONAUT_EXTENSIONS_PATH")) + "/fake/results";
    QFile::remove(log);
    auto logged = [&log] {
        QFile file(log);
        return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()) : QString();
    };
    auto command = [&](const QString &id) {
        for (const Extensions::Command &c : host->commands(m_dir, {})) {
            if (c.id == id) {
                host->invoke(c, m_dir, {});
                return true;
            }
        }
        return false;
    };
    QTRY_VERIFY_WITH_TIMEOUT(command("live"), 10000);

    // A command an extension put in the View menu: there (and so in the
    // command palette), not in the right-click menu; choosing it tells the
    // extension, with the open folder.
    {
        QMenu *view = w->m_extensionMenus.value("view");
        QVERIFY(view);
        auto inView = [&]() -> QAction * {
            for (QAction *action : view->actions()) {
                if (action->text() == "In The && Menu")
                    return action;
            }
            return nullptr;
        };
        QVERIFY(!inView());
        emit view->aboutToShow();
        QVERIFY(inView());
        emit view->aboutToShow(); // again: once, not twice
        int times = 0;
        for (QAction *action : view->actions())
            times += action->text() == "In The && Menu";
        QCOMPARE(times, 1);
        QMenu context;
        w->addExtensionCommands(&context, {});
        for (QAction *action : context.actions())
            QVERIFY(action->text() != "In The && Menu");
        QSignalSpy told(host, &Extensions::Host::message);
        inView()->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!told.isEmpty() && told.last().at(1).toString().startsWith("invoked inmenu on 0:"), 10000);
    }
    QTRY_VERIFY_WITH_TIMEOUT(w->quickLookVisible(), 10000);
    // Shown; the extension heard so and sent a version made for the room.
    QTRY_VERIFY_WITH_TIMEOUT(w->m_quickLook->shownText().startsWith("live 2 for "), 10000);
    QCOMPARE(w->m_quickLook->resultId(), QStringLiteral("live1"));
    QCOMPARE(logged(), QStringLiteral("live1 shown\n"));
    QVERIFY(!w->m_quickLook->shownText().contains(" 0x"));
    // With the theme's colours and font, to draw in.
    QVERIFY2(w->m_quickLook->shownText().contains("accent,border,dim,surface,text,window font=yes"),
             qPrintable(w->m_quickLook->shownText()));

    // A file takes the pane: the extension is told, and its next version is dropped.
    QTRY_VERIFY(w->m_pane->model->indexForPath(a).isValid());
    w->m_pane->view->setCurrentIndex(QModelIndex());
    w->m_pane->view->setCurrentIndex(w->toView(w->m_pane->model->indexForPath(a)));
    QTRY_COMPARE(w->m_quickLook->path(), a);
    QVERIFY(w->m_quickLook->resultId().isEmpty());
    QTRY_COMPARE_WITH_TIMEOUT(logged(), QStringLiteral("live1 shown\nlive1 hidden\n"), 10000);
    QSignalSpy said(host, &Extensions::Host::message);
    QVERIFY(command("late"));
    QTRY_VERIFY_WITH_TIMEOUT(!said.isEmpty(), 10000); // it has been through
    QTest::qWait(100);
    QCOMPARE(w->m_quickLook->path(), a);
    QVERIFY(!w->m_quickLook->shownText().contains("too late"));

    // Shown again, then the pane closes: told again, and an update doesn't open it.
    QVERIFY(command("live"));
    QTRY_VERIFY_WITH_TIMEOUT(w->m_quickLook->shownText().startsWith("live 2 for "), 10000);
    w->hideQuickLook();
    QVERIFY(!w->quickLookVisible());
    QTRY_VERIFY_WITH_TIMEOUT(logged().endsWith("live1 shown\nlive1 hidden\n"), 10000);
    said.clear();
    QVERIFY(command("late"));
    QTRY_VERIFY_WITH_TIMEOUT(!said.isEmpty(), 10000);
    QTest::qWait(100);
    QVERIFY(!w->quickLookVisible());
    host->setEnabled("fake", false);
}

// Extensions' commands in the menu: submenus, a tick, the text a command
// wants typed (nothing typed or Cancel: the extension isn't told), and a
// result shown in the preview pane.
void TestMainWindow::extensionMenusAskAndShow()
{
    if (QStandardPaths::findExecutable("python3").isEmpty())
        QSKIP("python3 isn't installed (the test extension is a Python script)");
    Extensions::Host *host = Extensions::Host::instance();
    host->setEnabled("fake", true);
    const QString a = make("a.txt");
    open(m_dir);
    auto findIn = [](QMenu *menu, const QString &text) -> QAction * {
        for (QAction *action : menu->actions()) {
            if (action->text() == text)
                return action;
        }
        return nullptr;
    };
    QMenu menu;
    QTRY_VERIFY_WITH_TIMEOUT([&] { menu.clear(); w->addExtensionCommands(&menu, {}); return findIn(&menu, "Fake") != nullptr; }(), 10000);
    // "Fake ▸ Sub ▸ Type & It…": two levels, the name shown as it is, ticked.
    QMenu *fake = findIn(&menu, "Fake")->menu();
    QVERIFY(fake && findIn(fake, "Show Me"));
    QMenu *sub = findIn(fake, "Sub")->menu();
    QVERIFY(sub);
    QAction *typed = findIn(sub, "Type && It…");
    QVERIFY(typed && typed->isChecked());

    // Types into the next text question and presses its button, or Cancel.
    QString label;
    auto answerText = [&](const QString &text, bool accept) {
        auto *timer = new QTimer(this);
        timer->setInterval(10);
        connect(timer, &QTimer::timeout, this, [&label, timer, text, accept] {
            auto *dialog = qobject_cast<QInputDialog *>(QApplication::activeModalWidget());
            if (!dialog)
                return;
            timer->stop();
            timer->deleteLater();
            label = dialog->labelText() + "|" + dialog->textValue() + "|" + dialog->okButtonText();
            dialog->setTextValue(text);
            accept ? dialog->accept() : dialog->reject();
        });
        timer->start();
    };
    QSignalSpy said(host, &Extensions::Host::message);
    answerText("ignored", false);
    typed->trigger();
    QCOMPARE(label, QStringLiteral("Say what:|default|Type & It")); // its label, its starting text, its verb
    answerText("   ", true); // nothing typed is "never mind"
    typed->trigger();
    QTest::qWait(400);
    QVERIFY(said.isEmpty());
    answerText("hello there", true);
    typed->trigger();
    QTRY_VERIFY_WITH_TIMEOUT(!said.isEmpty(), 10000);
    QCOMPARE(said.first().at(1).toString(), QStringLiteral("invoked typed on 0:  [hello there]"));

    // A result: the preview pane opens for it, with the diff as text.
    QVERIFY(!w->quickLookVisible());
    findIn(fake, "Show Me")->trigger();
    QTRY_VERIFY_WITH_TIMEOUT(w->quickLookVisible(), 10000);
    QTRY_VERIFY_WITH_TIMEOUT(w->m_quickLook->shownText().contains("+new line"), 10000);
    QVERIFY(w->m_quickLook->shownText().contains("-old <b>line</b>")); // shown, not obeyed
    QVERIFY(w->m_quickLook->path().isEmpty());
    // The next file selected takes the pane back.
    w->m_pane->view->setCurrentIndex(w->toView(w->m_pane->model->indexForPath(a)));
    QTRY_COMPARE(w->m_quickLook->path(), a);

    // A menu that is only one submenu shows what is inside it (the branch button).
    QMenu wrapped;
    wrapped.addMenu("Git")->addAction("Commit…");
    ExtensionMenu::flatten(&wrapped);
    QVERIFY(findIn(&wrapped, "Commit…"));
    QVERIFY(!findIn(&wrapped, "Git"));
    host->setEnabled("fake", false);
}

void TestMainWindow::diffIsColoured()
{
    QTextDocument doc;
    const QString diff = "diff --git a/x b/x\nindex 1..2\n--- a/x\n+++ b/x\n@@ -1,2 +1,2 @@\n same\n-old <i>x</i>\n+new\n";
    RichTextStyle::setDiff(&doc, diff, currentTheme());
    QCOMPARE(doc.toPlainText(), diff); // every character as it came
    auto background = [&](const QString &text) {
        for (QTextBlock b = doc.begin(); b.isValid(); b = b.next()) {
            if (b.text() == text)
                return b.blockFormat().background();
        }
        return QBrush(Qt::magenta);
    };
    QVERIFY(background("+new").style() != Qt::NoBrush);
    QVERIFY(background("-old <i>x</i>").style() != Qt::NoBrush);
    QVERIFY(background("+new").color() != background("-old <i>x</i>").color());
    QVERIFY(background("+new").color().green() > background("+new").color().red());
    QVERIFY(background("-old <i>x</i>").color().red() > background("-old <i>x</i>").color().green());
    QCOMPARE(background(" same").style(), Qt::NoBrush);
    QCOMPARE(background("+++ b/x").style(), Qt::NoBrush); // the file's name, not an added line
    QVERIFY(doc.begin().begin().fragment().charFormat().fontWeight() == QFont::Bold);
}

// A list of files an extension keeps: the pane goes there like to a place
// (title, back, rows for the files that exist), and the extension previews
// its files itself. Only full paths are taken; a list it never sent isn't shown.
void TestMainWindow::extensionListIsAPlace()
{
    if (QStandardPaths::findExecutable("python3").isEmpty())
        QSKIP("python3 isn't installed (the test extension is a Python script)");
    Extensions::Host *host = Extensions::Host::instance();
    host->setEnabled("fake", true);
    const QString a = make("a.txt"), b = make("b.txt");
    open(m_dir);
    QMenu menu;
    QAction *listMe = nullptr;
    QTRY_VERIFY_WITH_TIMEOUT([&] {
        menu.clear();
        w->addExtensionCommands(&menu, {});
        for (QAction *top : menu.actions()) {
            if (!top->menu())
                continue;
            for (QAction *action : top->menu()->actions()) {
                if (action->text() == "List Me")
                    listMe = action;
            }
        }
        return listMe != nullptr;
    }(), 10000);
    listMe->trigger();
    const QString place = Extensions::Host::listingPath("fake", "my list/1");
    QTRY_COMPARE_WITH_TIMEOUT(w->m_pane->current, place, 10000);
    QCOMPARE(w->titleFor(place), QStringLiteral("Fake list"));
    QCOMPARE(w->m_pathBar->path(), place);
    QTRY_VERIFY_WITH_TIMEOUT(w->m_pane->model->indexForPath(a).isValid(), 10000);
    QTRY_VERIFY(w->m_pane->model->indexForPath(b).isValid()); // "sub/../b.txt", cleaned
    QCOMPARE(w->m_pane->model->rowCount(), 2);                 // the missing and the relative one aren't rows
    QVERIFY(!w->m_pasteAct->isEnabled());                      // a place, not a folder to paste into

    // Space: the extension's own preview of the file, not the built-in one.
    w->m_pane->view->setCurrentIndex(w->toView(w->m_pane->model->indexForPath(a)));
    QTest::keyClick(w->m_pane->view, Qt::Key_Space);
    QTRY_VERIFY_WITH_TIMEOUT(w->quickLookVisible() && w->m_quickLook->fromExtension(), 10000);
    QCOMPARE(w->m_quickLook->shownText(), QStringLiteral("LISTING PREVIEW of a.txt in my list/1"));

    // Back to the folder; and with the extension off there is no such place.
    w->m_backAct->trigger();
    QCOMPARE(w->m_pane->current, m_dir);
    host->setEnabled("fake", false);
    QVERIFY(!w->navigateTo(place));
    QCOMPARE(w->m_pane->current, m_dir);
}

#include "tst_mainwindow.moc"
