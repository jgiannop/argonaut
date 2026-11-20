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
#include "PlanView.h"
#include "PdfPages.h"
#include "PdfView.h"
#include "MountActions.h"
#include "PathBar.h"
#include "QuickLook.h"
#include "TipPicture.h"
#include "TextFinder.h"
#include "RichTextStyle.h"
#include "Theme.h"
#include "Sidebar.h"
#include "SideTree.h"
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

#include <QPushButton>
#include <QClipboard>
#include <QMimeData>
#include <QPlainTextEdit>
#include <QPainter>
#include <QPageSize>
#include <QScrollBar>
#include <QPdfWriter>
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
#include <QScopeGuard>
#include <QSignalSpy>
#include <QSplitter>
#include <QTemporaryDir>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextTable>
#include <QTimer>
#include <QToolButton>
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
    void paintingABigFolderIsQuick();
    void pdfReaderInQuickLook();
    void previewFromAnExtension();
    void richTextGetsAPageLook();
    void terminalBringsTheFilesAlong();
    void terminalComesToTheFiles();
    void terminalPassesTheMouseOn();
    void extensionMenusAskAndShow();
    void diffIsColoured();
    void extensionListIsAPlace();
    void unmountedDriveInTheSidebar();
    void extensionPlanInThePane();
    void waitingForClaudeIsShown();
    void extensionResultStaysLive();
    void resultsStayInTabs();
    void longPictureFromAnExtensionScrolls();
    void pictureFromAnExtensionHasTooltips();
    void htmlPageInThePreview();
    void textInThePreviewIsFoundAndCopied();
    void textFileIsEditedInThePreview();
    void answerUnderAResult();
    void resultKeepsThePane();
    void extensionResultCanBeSearched();
    void extensionPlacesInTheSidebar();
    void extensionFormsAndAddresses();
    void themesFromFiles();
    void upAndDownButtons();
    void extensionTreeUnderThePlaces();
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

// Up selects the folder it left and Down goes back into it; a selected folder
// is where Down goes first.
void TestMainWindow::upAndDownButtons()
{
    make("a/b/c/file.txt");
    make("a/other/x.txt");
    const QString a = m_dir + "/a", b = a + "/b", c = b + "/c";
    open(c);
    QVERIFY(w->m_upButton && w->m_downButton);
    QVERIFY(w->m_upButton->isEnabled());
    QVERIFY(!w->m_downButton->isEnabled()); // nothing selected, nowhere to go back into

    w->m_upButton->click();
    QTRY_COMPARE(w->m_pane->current, b);
    QTRY_COMPARE(w->selectedPaths(), QStringList{c});
    QVERIFY(w->m_downButton->isEnabled());
    w->m_upButton->click();
    QTRY_COMPARE(w->m_pane->current, a);
    QTRY_COMPARE(w->selectedPaths(), QStringList{b});
    // Down twice leads back where Up started, even with the selection gone.
    w->m_pane->view->clearSelection();
    QTRY_VERIFY(w->m_downButton->isEnabled());
    w->m_downButton->click();
    QTRY_COMPARE(w->m_pane->current, b);
    QTRY_COMPARE(w->selectedPaths(), QStringList{c});
    w->m_downAct->trigger(); // Alt+Down
    QTRY_COMPARE(w->m_pane->current, c);
    QTRY_VERIFY(!w->m_downButton->isEnabled());

    // A selected folder comes before the trail.
    open(a);
    select({a + "/other"});
    QVERIFY(w->m_downAct->isEnabled());
    w->m_downAct->trigger();
    QTRY_COMPARE(w->m_pane->current, a + "/other");
    // A file selected is no way down.
    select({a + "/other/x.txt"});
    QVERIFY(!w->m_downAct->isEnabled());
    w->m_downAct->trigger();
    QCOMPARE(w->m_pane->current, a + "/other");
    // Going elsewhere forgets the trail.
    w->goUp();
    QTRY_COMPARE(w->m_pane->current, a);
    open(m_dir);
    open(a);
    w->m_pane->view->clearSelection();
    QTRY_VERIFY(!w->m_downAct->isEnabled());
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
// Painting the file view costs what the rows on screen cost, whatever the
// size of the folder, and nothing is asked of the disk for it: a repaint
// (a scroll step, a hover, the fade) happens many times a second. This
// guards against work that creeps into painting.
void TestMainWindow::paintingABigFolderIsQuick()
{
    for (int i = 0; i < 3000; ++i)
        make(QStringLiteral("item-%1.%2").arg(i, 4, 10, QChar('0')).arg(i % 3 == 0 ? "md" : i % 3 == 1 ? "csv" : "txt")); // nothing a thumbnailer is started for
    open(m_dir);
    QTRY_COMPARE_WITH_TIMEOUT(w->m_pane->model->rowCount(), 3000, 20000);
    for (const bool grid : {false, true}) {
        (grid ? w->m_gridAct : w->m_listAct)->trigger();
        QCoreApplication::processEvents();
        QAbstractItemView *view = w->m_pane->view;
        view->viewport()->grab(); // the first paint makes what is kept (icons, wrapped names)
        QElapsedTimer timer;
        timer.start();
        const int paints = 40;
        for (int i = 0; i < paints; ++i) {
            view->verticalScrollBar()->setValue(view->verticalScrollBar()->value() + 40); // other rows each time
            view->viewport()->grab();
        }
        const double each = timer.nsecsElapsed() / 1e6 / paints;
        qInfo("%s: %.2f ms a paint", grid ? "grid" : "list", each);
        // Far more than it takes here: only something badly wrong fails this.
        QVERIFY2(each < 150.0, qPrintable(QStringLiteral("%1 ms a paint").arg(each)));
    }
}

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
// Space on a PDF: a reader with every page, each drawn in the sandbox when
// it comes into sight. A file that only claims to be a PDF gets what any
// file gets.
void TestMainWindow::pdfReaderInQuickLook()
{
    if (!PdfPages::available())
        QSKIP("no PDF reader here: it needs pdfinfo and pdftoppm (poppler-utils) and a sandbox that works");
    // A real PDF of 40 pages, each with its number on it.
    const QString file = m_dir + "/long.pdf";
    {
        QPdfWriter writer(file);
        writer.setPageSize(QPageSize(QPageSize::A4));
        QPainter p(&writer);
        QFont font;
        font.setPointSize(48);
        p.setFont(font);
        for (int page = 1; page <= 40; ++page) {
            if (page > 1)
                writer.newPage();
            p.fillRect(QRect(0, 0, writer.width(), writer.height() / 8), QColor(page == 3 ? "#c01c28" : "#1c71d8"));
            p.drawText(QRect(0, 0, writer.width(), writer.height()), Qt::AlignCenter, QStringLiteral("Page number %1").arg(page));
        }
    }
    const QString broken = make("broken.pdf", "%PDF-1.4\nthis is not what a PDF goes on with\n");
    open(m_dir);
    auto current = [&](const QString &path) {
        w->m_pane->view->setCurrentIndex(w->toView(w->m_pane->model->indexForPath(path)));
    };
    current(file);
    QTest::keyClick(w->m_pane->view, Qt::Key_Space);
    QVERIFY(w->quickLookVisible());
    QuickLook *preview = w->m_quickLook;
    PdfView *reader = preview->pdfReader();
    QTRY_VERIFY_WITH_TIMEOUT(preview->isReadingPdf(), 15000);
    QCOMPARE(reader->pageCount(), 40);
    QCOMPARE(reader->currentPage(), 1);
    // Only what is in sight (and the next) is drawn: not forty pages.
    QTRY_VERIFY_WITH_TIMEOUT(reader->readyPages() >= 1, 15000);
    QTest::qWait(600);
    QVERIFY2(reader->readyPages() <= 4, qPrintable(QString::number(reader->readyPages())));
    QVERIFY(preview->hasOpenButton()); // it is a file: Open hands it to the PDF app
    // The page on screen is the PDF's own: blue across its top.
    QTRY_VERIFY_WITH_TIMEOUT([&] {
        const QImage seen = reader->viewport()->grab().toImage();
        return seen.pixelColor(seen.width() / 2, 24) == QColor("#1c71d8");
    }(), 15000);
    // Further on: the page says where we are, and the pages there are drawn.
    reader->showPage(3);
    QCOMPARE(reader->currentPage(), 3);
    QTRY_VERIFY_WITH_TIMEOUT([&] {
        const QImage seen = reader->viewport()->grab().toImage();
        return seen.pixelColor(seen.width() / 2, 24) == QColor("#c01c28"); // page 3 is the red one
    }(), 15000);
    reader->showPage(40);
    QCOMPARE(reader->currentPage(), 40);
    QTRY_VERIFY_WITH_TIMEOUT(reader->readyPages() >= 2, 15000);
    if (qEnvironmentVariableIsSet("ARGONAUT_TEST_GRAB")) { // to look at it
        reader->showPage(2);
        QTest::qWait(1500);
        w->grab().save(qEnvironmentVariable("ARGONAUT_TEST_GRAB"));
    }
    // Bigger: the same page stays, wider than the pane now.
    reader->showPage(5);
    reader->setZoom(2);
    QCOMPARE(reader->currentPage(), 5);
    QVERIFY(reader->horizontalScrollBar()->maximum() > 0);
    reader->setZoom(0.2); // never smaller than fitted
    QCOMPARE(reader->zoom(), 1.0);

    // A file that only starts like a PDF: no reader, the usual preview.
    current(broken);
    QTRY_VERIFY_WITH_TIMEOUT(!preview->isReadingPdf() && reader->pageCount() == 0, 15000);
    // Back on the real one, then away: the reader lets go of the file.
    current(file);
    QTRY_VERIFY_WITH_TIMEOUT(preview->isReadingPdf() && reader->pageCount() == 40, 15000);
    QCOMPARE(reader->currentPage(), 1); // from the start again
    w->hideQuickLook();

    // A reader that goes away while its pages are still being drawn isn't
    // waited for, and what comes back for it afterwards harms nothing.
    QElapsedTimer timer;
    for (int i = 0; i < 5; ++i) {
        auto *gone = new PdfView;
        gone->resize(400, 600);
        gone->show();
        gone->setFile(file);
        if (i % 2)
            QTest::qWait(30); // sometimes just as the first answer is on its way
        timer.start();
        delete gone;
        QVERIFY2(timer.elapsed() < 500, qPrintable(QString::number(timer.elapsed())));
    }
    QTest::qWait(600); // what was started finishes, with nobody left to tell
}

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

// A program that asks for the mouse (Claude Code: the alternate screen and
// SGR mouse reports) scrolls itself: the wheel, clicks and drags go to it,
// Shift+click doesn't. One on the alternate screen without the mouse (less)
// gets arrow keys for the wheel.
void TestMainWindow::terminalPassesTheMouseOn()
{
#if defined(ARGONAUT_TERMINAL) && defined(Q_OS_LINUX)
    open(m_dir);
    w->m_terminalAct->trigger();
    TerminalWidget *terminal = w->m_terminal;
    QVERIFY(terminal && terminal->isRunning());
    QTRY_VERIFY_WITH_TIMEOUT(terminal->isShellIdle(), 10000);
    terminal->resize(600, 300);
    const QString mouse = m_dir + "/mouse.bytes";
    const QString keys = m_dir + "/keys.bytes";
    // What it is sent is written down as it comes (raw: nothing is changed on the way).
    const QString script = m_dir + "/asks.sh";
    TestSupport::writeScript(script, "#!/bin/sh\n"
                                     "printf '\\033[?1049h\\033[?1002h\\033[?1006h'\n"
                                     "stty raw -echo\n"
                                     "timeout --foreground 3 cat > \"$1\"\n"
                                     "printf '\\033[?1002l\\033[?1006l'\n"
                                     "timeout --foreground 3 cat > \"$2\"\n"
                                     "stty sane\n"
                                     "printf '\\033[?1049l'\n");
    QTest::keyClicks(terminal, QStringLiteral("sh '%1' '%2' '%3'").arg(script, mouse, keys));
    QTest::keyClick(terminal, Qt::Key_Return);
    QTRY_VERIFY_WITH_TIMEOUT(terminal->programWantsMouse() && terminal->onAlternateScreen(), 10000);
    QTest::qWait(200); // cat is reading
    auto read = [](const QString &path) {
        QFile file(path);
        return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
    };
    auto wheel = [terminal](int delta, Qt::KeyboardModifiers mods = Qt::NoModifier) {
        const QPointF at(50, 40);
        QWheelEvent event(at, terminal->mapToGlobal(at), QPoint(), QPoint(0, delta), Qt::NoButton, mods, Qt::NoScrollPhase, false);
        QApplication::sendEvent(terminal, &event);
    };
    wheel(120);  // up: button 64 in SGR
    wheel(-60);  // half a notch (a touchpad)…
    wheel(-60);  // …and the other half: down, button 65
    QTest::mouseClick(terminal, Qt::LeftButton, Qt::NoModifier, QPoint(50, 40));
    QTest::mouseClick(terminal, Qt::LeftButton, Qt::ShiftModifier, QPoint(50, 40)); // ours: selects
    QTRY_VERIFY_WITH_TIMEOUT(read(mouse).contains("\x1b[<0;") && read(mouse).contains("m"), 5000);
    const QByteArray sent = read(mouse);
    QVERIFY2(sent.contains("\x1b[<64;"), sent.toHex(' ').constData());
    QVERIFY2(sent.contains("\x1b[<65;"), sent.toHex(' ').constData());
    QCOMPARE(sent.count("\x1b[<0;"), 2); // the click's press and release, not the Shift+click
    QVERIFY(sent.indexOf("\x1b[<64;") < sent.indexOf("\x1b[<65;"));

    // The mouse given back, still on the alternate screen: the wheel is arrow keys.
    QTRY_VERIFY_WITH_TIMEOUT(!terminal->programWantsMouse(), 10000);
    QTest::qWait(200);
    wheel(-120);
    QTRY_VERIFY_WITH_TIMEOUT(read(keys).count("\x1b[B") + read(keys).count("\x1bOB") == 3, 5000);
    QVERIFY(!read(keys).contains("\x1b[<"));
    QTRY_VERIFY_WITH_TIMEOUT(!terminal->onAlternateScreen() && terminal->isShellIdle(), 10000);
    w->m_terminalAct->trigger();
#else
    QSKIP("no embedded terminal in this build");
#endif
}

// The other way round: Ctrl+` (or the header's button) while the terminal is
// open and in another folder brings its shell to the pane's folder and
// leaves it open; in the same folder it closes the panel, as before.
void TestMainWindow::terminalComesToTheFiles()
{
#if defined(ARGONAUT_TERMINAL) && defined(Q_OS_LINUX)
    const QString other = m_dir + "/else where";
    QDir().mkpath(other);
    open(m_dir);
    w->m_terminalAct->trigger();
    TerminalWidget *terminal = w->m_terminal;
    QVERIFY(terminal && terminal->isRunning());
    QTRY_VERIFY_WITH_TIMEOUT(terminal->isShellIdle() && terminal->shellDirectory() == m_dir, 10000);
    QVERIFY(!w->m_terminalPanel->isHidden());

    // The files go elsewhere; the terminal stays where it is...
    open(other);
    QCOMPARE(terminal->shellDirectory(), m_dir);
    // ...until the key: it comes along, and the panel stays open and ticked.
    w->m_terminalAct->trigger();
    QVERIFY(!w->m_terminalPanel->isHidden());
    QTRY_VERIFY(w->m_terminalAct->isChecked() && w->m_terminalButton->isChecked());
    QTRY_COMPARE_WITH_TIMEOUT(terminal->shellDirectory(), other, 10000);
    QCOMPARE(w->m_pane->current, other);
    QVERIFY(!w->m_terminalPanel->isHidden());
    // The "cd" typed for it isn't left in the terminal, and the picture is shown again.
    QTRY_VERIFY_WITH_TIMEOUT(terminal->updatesEnabled(), 5000);
    QVERIFY2(!terminal->screenText().contains("cd --"), qPrintable(terminal->screenText()));
    QVERIFY2(!terminal->screenText().trimmed().isEmpty(), qPrintable(terminal->screenText()));
    // The header's button does the same.
    QTRY_VERIFY_WITH_TIMEOUT(terminal->isShellIdle(), 10000);
    open(m_dir);
    w->m_terminalButton->click();
    QVERIFY(!w->m_terminalPanel->isHidden());
    QTRY_VERIFY(w->m_terminalAct->isChecked() && w->m_terminalButton->isChecked());
    QTRY_COMPARE_WITH_TIMEOUT(terminal->shellDirectory(), m_dir, 10000);
    QVERIFY(!w->m_terminalPanel->isHidden());
    // In the same folder: the key closes the panel, as it always did.
    QTRY_VERIFY_WITH_TIMEOUT(terminal->isShellIdle(), 10000);
    w->m_terminalAct->trigger();
    QVERIFY(w->m_terminalPanel->isHidden());
    QVERIFY(!w->m_terminalAct->isChecked() && !w->m_terminalButton->isChecked());
    // Opened again from another folder: it comes there (that is not new).
    open(other);
    w->m_terminalAct->trigger();
    QVERIFY(!w->m_terminalPanel->isHidden());
    QTRY_COMPARE_WITH_TIMEOUT(terminal->shellDirectory(), other, 10000);
    // ✕ on the panel always hides it, wherever the shell is.
    QTRY_VERIFY_WITH_TIMEOUT(terminal->isShellIdle(), 10000);
    open(m_dir);
    w->m_terminalCloseButton->click();
    QVERIFY(w->m_terminalPanel->isHidden());
    QVERIFY(!w->m_terminalAct->isChecked() && !w->m_terminalButton->isChecked());
    QCOMPARE(terminal->shellDirectory(), other);
    // In a place that isn't a folder the key closes too (there is nowhere to come to).
    w->m_terminalAct->trigger();
    QTRY_COMPARE_WITH_TIMEOUT(terminal->shellDirectory(), m_dir, 10000);
    QTRY_VERIFY_WITH_TIMEOUT(terminal->isShellIdle(), 10000);
    QVERIFY(w->navigateTo(DirectoryModel::recentPath()));
    w->m_terminalAct->trigger();
    QVERIFY(w->m_terminalPanel->isHidden());
    QCOMPARE(terminal->shellDirectory(), m_dir);
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
// An extension's tree (Project Explorer's project) under the places, with a
// splitter: faint and left-out paths as it says, a click on a folder opens
// it, on a file shows it selected in its folder; the ✕ closes it.
void TestMainWindow::extensionTreeUnderThePlaces()
{
    if (QStandardPaths::findExecutable("python3").isEmpty())
        QSKIP("python3 isn't installed (the test extension is a Python script)");
    Extensions::Host *host = Extensions::Host::instance();
    make("build/out.o");
    make(".git/config");
    make("ok/deep/x.txt");
    make("src/main.cpp");
    make("README.md");
    open(m_dir);
    QVERIFY(w->m_sideTree && !w->m_sideTree->isVisible());
    host->setEnabled("fake", true);
    Extensions::Command tree;
    QTRY_VERIFY_WITH_TIMEOUT([&] {
        for (const Extensions::Command &c : host->commands(m_dir, {}))
            if (c.extension == "fake" && c.id == "tree")
                tree = c;
        return !tree.id.isEmpty();
    }(), 10000);
    host->invoke(tree, m_dir, {});
    QTRY_VERIFY_WITH_TIMEOUT(w->m_sideTree->isVisible(), 10000);
    QCOMPARE(w->m_sideSplit->indexOf(w->m_sideTree), 1); // under the places
    QCOMPARE(w->m_sideTree->tree().folder, m_dir);

    // Listed: faint where the extension says, .git left out.
    QTreeView *view = w->m_sideTree->view();
    QTRY_VERIFY_WITH_TIMEOUT(w->m_sideTree->indexOf(m_dir + "/src").isValid()
                                 && view->model()->rowCount(view->rootIndex()) == 4, 10000); // build, ok, src, README.md
    QVERIFY(!w->m_sideTree->indexOf(m_dir + "/.git").isValid());
    const QModelIndex build = w->m_sideTree->indexOf(m_dir + "/build");
    QVERIFY(w->m_sideTree->isFaint(build));
    QVERIFY(build.data(Qt::ForegroundRole).value<QColor>().isValid());
    QVERIFY(!w->m_sideTree->isFaint(w->m_sideTree->indexOf(m_dir + "/src")));
    QVERIFY(!w->m_sideTree->indexOf(m_dir + "/src").data(Qt::ForegroundRole).value<QColor>().isValid());
    // Folders first.
    QCOMPARE(w->m_sideTree->pathAt(view->model()->index(3, 0, view->rootIndex())), m_dir + "/README.md");

    // A click on a folder opens it in the files; on a file, its folder with it selected.
    emit view->clicked(w->m_sideTree->indexOf(m_dir + "/src"));
    QTRY_COMPARE(w->m_pane->current, m_dir + "/src");
    QVERIFY(view->isExpanded(w->m_sideTree->indexOf(m_dir + "/src")));
    QTRY_VERIFY_WITH_TIMEOUT(w->m_sideTree->indexOf(m_dir + "/src/main.cpp").isValid(), 5000);
    open(m_dir);
    emit view->clicked(w->m_sideTree->indexOf(m_dir + "/src/main.cpp"));
    QTRY_COMPARE(w->m_pane->current, m_dir + "/src");
    QTRY_COMPARE(w->selectedPaths(), QStringList{m_dir + "/src/main.cpp"});
    // Going to a folder in the files marks its row.
    open(m_dir + "/ok");
    QCOMPARE(w->m_sideTree->pathAt(view->currentIndex()), m_dir + "/ok");
    // The keys stay with the files.
    QCOMPARE(view->focusPolicy(), Qt::NoFocus);

    // The ✕: gone, and the extension is told.
    QSignalSpy said(host, &Extensions::Host::message);
    emit w->m_sideTree->closeRequested();
    QVERIFY(!w->m_sideTree->isVisible());
    QVERIFY(!host->tree().valid);
    QTRY_VERIFY_WITH_TIMEOUT(!said.isEmpty() && said.last().at(1).toString() == "tree closed t1", 10000);
    // Switched off with a tree shown: it goes too.
    host->invoke(tree, m_dir, {});
    QTRY_VERIFY_WITH_TIMEOUT(w->m_sideTree->isVisible(), 10000);
    host->setEnabled("fake", false);
    QVERIFY(!w->m_sideTree->isVisible());
}

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

// A long picture from an extension can have parts with a tooltip (the ⓘ
// beside a service): the mouse on one, or a press, shows its text.
void TestMainWindow::pictureFromAnExtensionHasTooltips()
{
    open(m_dir);
    QImage tall(300, 2000, QImage::Format_RGB32);
    tall.fill(QColor("#336699"));
    const QString file = m_dir + "/tipped.png";
    QVERIFY(tall.save(file));
    Extensions::PreviewReply content;
    content.kind = Extensions::PreviewReply::Image;
    content.image = file;
    content.extension = "fake";
    content.resultId = "tipped";
    content.scroll = true;
    content.tips = {{QRect(40, 50, 24, 24), "About the <b>first</b>\nsecond line"}, {QRect(40, 1500, 24, 24), "About the last"}};
    w->showExtensionResult("fake", "Tipped", content);
    QVERIFY(w->m_quickLook && w->m_quickLook->isVisible());
    TipPicture *picture = w->m_quickLook->tipPicture();
    QTRY_COMPARE(picture->tipCount(), 2);
    const QPoint first = picture->tipArea(0).center();
    QCOMPARE(picture->tipAt(first), QStringLiteral("About the <b>first</b>\nsecond line"));
    QCOMPARE(picture->tipAt(picture->tipArea(1).center()), QStringLiteral("About the last"));
    QCOMPARE(picture->tipAt(QPoint(200, 60)), QString()); // beside it: nothing
    QCOMPARE(picture->tipAt(picture->tipArea(0).bottomRight() + QPoint(3, 3)), QString());
    // The mouse resting on it (what the toolkit sends then), and a press, show it.
    QHelpEvent rest(QEvent::ToolTip, first, picture->mapToGlobal(first));
    QApplication::sendEvent(picture, &rest);
    QCOMPARE(picture->shownTip(), QStringLiteral("About the <b>first</b>\nsecond line"));
    QTest::mouseClick(picture, Qt::LeftButton, Qt::NoModifier, QPoint(200, 60));
    QCOMPARE(picture->shownTip(), QString());
    QTest::mouseClick(picture, Qt::LeftButton, Qt::NoModifier, picture->tipArea(1).center());
    QCOMPARE(picture->shownTip(), QStringLiteral("About the last"));
    // The pointer is a hand over a sign, and not beside it.
    QTest::mouseMove(picture, first);
    QTRY_COMPARE(picture->cursor().shape(), Qt::PointingHandCursor);
    QTest::mouseMove(picture, QPoint(200, 60));
    QTRY_VERIFY(picture->cursor().shape() != Qt::PointingHandCursor);
    // The same result made again without them: none are left from before.
    content.update = true;
    content.tips.clear();
    w->showExtensionResult("fake", "Tipped", content);
    QCOMPARE(picture->tipCount(), 0);
    QCOMPARE(picture->tipAt(first), QString());
}

// An HTML file's page (what Document Preview sends for one) is shown as a
// page in the pane: its text is there to read, and nothing of it is loaded
// or followed.
void TestMainWindow::htmlPageInThePreview()
{
    open(m_dir);
    Extensions::PreviewReply content;
    content.kind = Extensions::PreviewReply::Html;
    content.extension = "document-preview";
    content.text = "<h1>A Heading</h1><p>Some <b>bold</b> text and a <a href=\"http://example.com/x\">link</a>.</p>"
                   "<ul><li>One</li><li>Two</li></ul><table><tr><th>name</th></tr><tr><td>cell</td></tr></table>"
                   "<img src=\"http://example.com/never-fetched.png\">";
    if (qEnvironmentVariableIsSet("ARGONAUT_TEST_HTML")) { // a real page, to look at it
        QFile real(qEnvironmentVariable("ARGONAUT_TEST_HTML"));
        if (real.open(QIODevice::ReadOnly))
            content.text = QString::fromUtf8(real.readAll());
    }
    content.note = "As a page to read: without its styles and scripts";
    w->showExtensionResult("document-preview", "page.html", content);
    QVERIFY(w->m_quickLook && w->m_quickLook->isVisible());
    QuickLook *preview = w->m_quickLook;
    QVERIFY(preview->fromExtension());
    if (qEnvironmentVariableIsSet("ARGONAUT_TEST_GRAB")) {
        w->resize(1200, 800);
        QTest::qWait(600);
        w->grab().save(qEnvironmentVariable("ARGONAUT_TEST_GRAB"));
        return;
    }
    const QString shown = preview->shownText();
    QVERIFY2(shown.contains("A Heading") && shown.contains("Some bold text and a link.") && shown.contains("One") && shown.contains("cell"),
             qPrintable(shown));
    QVERIFY2(!shown.contains("<h1>") && !shown.contains("<b>"), qPrintable(shown)); // a page, not its source
}

// A text in the preview: a click gives it the keys, and then it can be
// selected and copied, and Ctrl+F finds in it. The window's shortcuts that
// act on files (Delete, Ctrl+C, Ctrl+A) don't while the keys are there; Esc
// gives them back to the files.
void TestMainWindow::textInThePreviewIsFoundAndCopied()
{
    const QString a = make("notes.txt", "first line\nthe Needle is here\nnothing\na needle again, and NEEDLE\nlast\n");
    open(m_dir);
    w->m_pane->view->setCurrentIndex(w->toView(w->m_pane->model->indexForPath(a)));
    QTest::keyClick(w->m_pane->view, Qt::Key_Space);
    QuickLook *preview = w->m_quickLook;
    QTRY_COMPARE(preview->kind(), Preview::Kind::Text);
    QPlainTextEdit *text = preview->textView();
    QTRY_VERIFY(text->toPlainText().contains("Needle"));
    w->show();
    QVERIFY(QTest::qWaitForWindowExposed(w.get()));
    QTest::mouseClick(text->viewport(), Qt::LeftButton, {}, QPoint(10, 10));
    QTRY_VERIFY(text->hasFocus());

    // Delete and Ctrl+A are the text's now: the file isn't trashed, the text is selected.
    QTest::keyClick(text, Qt::Key_Delete);
    QTest::qWait(300);
    QVERIFY(QFile::exists(a));
    QTest::keyClick(text, Qt::Key_A, Qt::ControlModifier);
    QCOMPARE(text->textCursor().selectedText().replace(QChar::ParagraphSeparator, '\n'), text->toPlainText());
    QApplication::clipboard()->clear();
    QTest::keyClick(text, Qt::Key_C, Qt::ControlModifier);
    QCOMPARE(QApplication::clipboard()->text(), text->toPlainText()); // the words, not the file
    QVERIFY(QApplication::clipboard()->mimeData()->urls().isEmpty());

    // Ctrl+F: the bar, finding as it is typed, any case.
    QTextCursor start = text->textCursor();
    start.clearSelection();
    start.movePosition(QTextCursor::Start);
    text->setTextCursor(start);
    TextFinder *finder = preview->finder();
    QVERIFY(!finder->isVisible());
    QTest::keyClick(text, Qt::Key_F, Qt::ControlModifier);
    QVERIFY(finder->isVisible());
    QVERIFY(!w->m_pane->model->isSearch()); // not the window's search
    QTRY_VERIFY(finder->field()->hasFocus());
    QTest::keyClicks(finder->field(), "needle");
    QCOMPARE(finder->count(), 3);
    QCOMPARE(finder->current(), 0);
    QCOMPARE(finder->countText(), QStringLiteral("1 of 3"));
    QCOMPARE(text->textCursor().selectedText(), QStringLiteral("Needle"));
    QTest::keyClick(finder->field(), Qt::Key_Return);
    QCOMPARE(finder->current(), 1);
    QCOMPARE(text->textCursor().selectedText(), QStringLiteral("needle"));
    QTest::keyClick(finder->field(), Qt::Key_Return, Qt::ShiftModifier);
    QTest::keyClick(finder->field(), Qt::Key_Return, Qt::ShiftModifier);
    QCOMPARE(finder->current(), 2); // round to the last
    QCOMPARE(text->textCursor().selectedText(), QStringLiteral("NEEDLE"));
    QCOMPARE(text->extraSelections().size(), 3); // every one is marked
    QTest::keyClicks(finder->field(), "x");
    QCOMPARE(finder->countText(), QStringLiteral("No matches"));
    // Esc: the bar goes with its marks, the keys go back to the text; Esc again, to the files.
    QTest::keyClick(finder->field(), Qt::Key_Escape);
    QVERIFY(!finder->isVisible());
    QVERIFY(text->extraSelections().isEmpty());
    QTRY_VERIFY(text->hasFocus());
    QTest::keyClick(text, Qt::Key_Escape);
    QTRY_VERIFY(w->m_pane->view->hasFocus());
    QVERIFY(w->quickLookVisible());
    QVERIFY(QFile::exists(a));
}

// Ctrl+Space (or the Edit box): the selected text file can be edited in the
// preview. The keys are the editor's (Delete, Ctrl+Z don't touch files);
// Ctrl+S saves, and the first save puts the version from before in the
// Trash. The file being edited keeps the pane; unsaved changes are asked
// about before it closes. What isn't text can't be edited.
void TestMainWindow::textFileIsEditedInThePreview()
{
    const QString a = make("letter.txt", "Dear friend,\n");
    const QString b = make("other.txt", "other\n");
    const QString data = make("data.bin", QByteArray("ab\0cd", 5));
    open(m_dir);
    w->show();
    QVERIFY(QTest::qWaitForWindowExposed(w.get()));
    auto current = [&](const QString &path) {
        w->m_pane->view->setCurrentIndex(w->toView(w->m_pane->model->indexForPath(path)));
    };
    auto trashed = [] { return QDir(Trash::homeFilesDir()).entryList(QDir::Files | QDir::NoDotAndDotDot); };
    auto read = [](const QString &path) {
        QFile file(path);
        return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
    };
    const int trashedBefore = int(trashed().size());

    // What isn't text: the pane says why, and nothing is editable.
    current(data);
    QTest::keyClick(w->m_pane->view, Qt::Key_Space, Qt::ControlModifier);
    QVERIFY(w->quickLookVisible());
    QuickLook *preview = w->m_quickLook;
    QVERIFY(!preview->isEditing());
    QVERIFY2(preview->noteText().contains("Only text files"), qPrintable(preview->noteText()));
    QVERIFY(preview->editBox()->isHidden()); // not even offered
    // Nor for a picture.
    QImage picture(8, 8, QImage::Format_RGB32);
    picture.fill(Qt::red);
    QVERIFY(picture.save(m_dir + "/photo.png"));
    QTRY_VERIFY(w->m_pane->model->indexForPath(m_dir + "/photo.png").isValid());
    current(m_dir + "/photo.png");
    QTRY_COMPARE(preview->path(), m_dir + "/photo.png");
    QVERIFY(preview->editBox()->isHidden());
    // A text file that isn't UTF-8 is offered, and ticking it says why it can't be.
    const QString latin = make("latin.txt", "caf\xe9\n");
    QTRY_VERIFY(w->m_pane->model->indexForPath(latin).isValid());
    current(latin);
    QTRY_COMPARE(preview->path(), latin);
    QVERIFY(!preview->editBox()->isHidden());
    preview->editBox()->setChecked(true);
    QVERIFY(!preview->isEditing() && !preview->editBox()->isChecked());
    QVERIFY2(preview->noteText().contains("isn't UTF-8 text"), qPrintable(preview->noteText()));

    // Ctrl+Space on the letter: editing, the cursor in the text.
    current(a);
    QTest::keyClick(w->m_pane->view, Qt::Key_Space, Qt::ControlModifier);
    QTRY_VERIFY(preview->isEditing());
    QPlainTextEdit *text = preview->textView();
    QVERIFY(!text->isReadOnly());
    QVERIFY(preview->editBox()->isChecked());
    QCOMPARE(text->toPlainText(), QStringLiteral("Dear friend,\n"));
    QTRY_VERIFY(text->hasFocus());
    // On white paper, like a notepad, whatever the theme.
    auto paper = [&] { return text->viewport()->grab().toImage().pixelColor(text->viewport()->width() - 4, text->viewport()->height() - 4); };
    QTRY_COMPARE(paper(), QColor(Qt::white));
    text->moveCursor(QTextCursor::End);
    QTest::keyClicks(text, "Thanks!x");
    QTest::keyClick(text, Qt::Key_Backspace);
    QTest::keyClick(text, Qt::Key_Delete); // nothing after the cursor: nothing happens, to the files either
    QTest::qWait(200);
    QVERIFY(QFile::exists(a));
    QCOMPARE(text->toPlainText(), QStringLiteral("Dear friend,\nThanks!"));
    // A preview that was still loading doesn't replace what is typed.
    QTest::qWait(200);
    QCOMPARE(text->toPlainText(), QStringLiteral("Dear friend,\nThanks!"));

    // Ctrl+S: saved; the version from before is in the Trash, once.
    QTest::keyClick(text, Qt::Key_S, Qt::ControlModifier);
    QCOMPARE(read(a), QByteArray("Dear friend,\nThanks!"));
    QCOMPARE(int(trashed().size()), trashedBefore + 1);
    QTest::keyClicks(text, " Bye");
    QTest::keyClick(text, Qt::Key_S, Qt::ControlModifier);
    QCOMPARE(read(a), QByteArray("Dear friend,\nThanks! Bye"));
    QCOMPARE(int(trashed().size()), trashedBefore + 1); // not a version a save

    // Another file selected meanwhile doesn't take the pane.
    current(b);
    QTest::qWait(100);
    QCOMPARE(preview->path(), a);
    QVERIFY(preview->isEditing());

    // Unsaved changes: closing the pane asks; Cancel keeps editing, Discard closes it.
    QTest::keyClicks(text, " unsaved");
    answerNext("Cancel");
    QVERIFY(!w->hideQuickLook());
    QVERIFY(w->quickLookVisible() && preview->isEditing());
    QVERIFY(m_question.contains("Save the changes"));
    answerNext("Discard");
    QVERIFY(w->hideQuickLook());
    QVERIFY(!w->quickLookVisible());
    QCOMPARE(read(a), QByteArray("Dear friend,\nThanks! Bye"));

    // Another program changes the file while it is edited: asked before it is saved over.
    current(a);
    w->editInQuickLook();
    QTRY_VERIFY(preview->isEditing());
    QTest::qWait(20);
    {
        QFile other(a);
        QVERIFY(other.open(QIODevice::WriteOnly | QIODevice::Truncate));
        other.write("their words\n");
    }
    text->moveCursor(QTextCursor::End);
    QTest::keyClicks(text, "!");
    answerNext("Cancel");
    QVERIFY(!preview->saveEditing());
    QCOMPARE(read(a), QByteArray("their words\n")); // theirs, untouched
    answerNext("Save Mine");
    QVERIFY(preview->saveEditing());
    QCOMPARE(read(a), QByteArray("Dear friend,\nThanks! Bye!"));
    QCOMPARE(int(trashed().size()), trashedBefore + 2); // theirs is in the Trash

    // Unticking Edit: the preview again, read-only.
    preview->editBox()->setChecked(false);
    QVERIFY(!preview->isEditing());
    QTRY_VERIFY(text->isReadOnly());
    QVERIFY(text->styleSheet().isEmpty()); // the preview's own look again
}

// What an extension has to say about its result on screen (Claude's report
// on the list of services) is shown in a part of the pane under it: the
// result stays where it is. Waiting for Claude is said there too.
void TestMainWindow::answerUnderAResult()
{
    const QString a = make("a.txt");
    open(m_dir);
    Extensions::PreviewReply list;
    list.kind = Extensions::PreviewReply::Text;
    list.text = "the list";
    list.extension = "fake";
    list.resultId = "list1";
    list.actionId = "analyze";
    list.actionLabel = "Analyze";
    list.actionInput = true;
    w->showExtensionResult("fake", "The List", list);
    QVERIFY(w->m_quickLook && w->m_quickLook->isVisible());
    QuickLook *pane = w->m_quickLook;
    QCOMPARE(pane->resultId(), QStringLiteral("list1"));
    QVERIFY(!pane->isUnderShown());

    // Claude is asked for this extension: the list stays, and under it the pane says it is waiting.
    w->showClaudeWaiting("fake", "Fake", 1);
    QVERIFY(pane->isUnderShown());
    QVERIFY(!pane->isWorking() && !pane->isHeld()); // the pane isn't taken
    QCOMPARE(pane->shownText(), QStringLiteral("the list"));
    QCOMPARE(pane->resultId(), QStringLiteral("list1"));
    QVERIFY2(pane->underWaitingText().startsWith("Waiting for Claude's answer."), qPrintable(pane->underWaitingText()));
    QVERIFY(pane->underText().isEmpty());
    QVERIFY(pane->underStopButton()->isVisibleTo(pane));
    QVERIFY(pane->hasAction()); // the button and its field are still there
    QTRY_VERIFY_WITH_TIMEOUT(pane->underWaitingText().contains("seconds so far"), 6000);
    // The list is made again meanwhile (it does that every few seconds): nothing changes below.
    Extensions::PreviewReply again = list;
    again.update = true;
    again.text = "the list, again";
    w->showExtensionResult("fake", "The List", again);
    QCOMPARE(pane->shownText(), QStringLiteral("the list, again"));
    QVERIFY(!pane->underWaitingText().isEmpty());
    // Stop Waiting asks for that.
    QSignalSpy stop(pane, &QuickLook::workingStopRequested);
    pane->underStopButton()->click();
    QCOMPARE(stop.size(), 1);
    QCOMPARE(stop.first().at(0).toString(), QStringLiteral("fake"));
    // Nothing came: the lower part closes, the list is as it was.
    w->showClaudeWaiting("fake", "Fake", 0);
    QVERIFY(!pane->isUnderShown());
    QCOMPARE(pane->shownText(), QStringLiteral("the list, again"));

    // The answer: under the list, as a page; the list stays.
    w->showClaudeWaiting("fake", "Fake", 1);
    w->showClaudeWaiting("fake", "Fake", 0);
    Extensions::PreviewReply report;
    report.kind = Extensions::PreviewReply::Markdown;
    report.text = "# Report\n\nAll **fine**. ![x](file:///etc/passwd)";
    report.extension = "fake";
    report.resultId = "report1";
    report.under = "list1";
    report.note = "this cost about $0.020";
    report.actionId = "back"; // only for where it is shown on its own
    report.actionLabel = "Back";
    w->showExtensionResult("fake", "Claude's Report", report);
    QVERIFY(pane->isUnderShown());
    QCOMPARE(pane->underTitle(), QStringLiteral("Claude's Report"));
    QVERIFY2(pane->underText().contains("Report") && pane->underText().contains("All fine."), qPrintable(pane->underText()));
    QVERIFY(!pane->underText().contains("**") && !pane->underText().contains("# "));
    QCOMPARE(pane->underNote(), QStringLiteral("this cost about $0.020"));
    QVERIFY(pane->underWaitingText().isEmpty());
    QCOMPARE(pane->resultId(), QStringLiteral("list1")); // the list is still the result
    QCOMPARE(pane->shownText(), QStringLiteral("the list, again"));
    QCOMPARE(pane->actionButton()->text(), QStringLiteral("Analyze")); // and its button, not the report's
    if (qEnvironmentVariableIsSet("ARGONAUT_TEST_GRAB")) { // to look at it
        w->resize(1000, 760);
        QTest::qWait(300);
        w->grab().save(qEnvironmentVariable("ARGONAUT_TEST_GRAB"));
    }
    // Asked again: the waiting line takes the lower part; an error brings the earlier answer back.
    w->showClaudeWaiting("fake", "Fake", 1);
    QVERIFY(!pane->underWaitingText().isEmpty() && pane->underText().isEmpty());
    w->showClaudeWaiting("fake", "Fake", 0);
    QVERIFY2(pane->underText().contains("All fine."), qPrintable(pane->underText()));
    // Its ✕ closes that part only.
    pane->closeUnder();
    QVERIFY(!pane->isUnderShown());
    QCOMPARE(pane->resultId(), QStringLiteral("list1"));

    // Another extension's waiting isn't about this list: it takes the pane, as before.
    w->showClaudeWaiting("another", "Another", 1);
    QVERIFY(pane->isWorking() && !pane->isUnderShown());
    w->showClaudeWaiting("another", "Another", 0);

    // What is meant for a result that isn't on screen is a result of its own.
    w->showExtensionResult("fake", "The List", list);
    report.under = "some-other-list";
    w->showExtensionResult("fake", "Claude's Report", report);
    QVERIFY(!pane->isUnderShown());
    QCOMPARE(pane->resultId(), QStringLiteral("report1"));
    QVERIFY(pane->shownText().contains("All fine."));
    QCOMPARE(pane->actionButton()->text(), QStringLiteral("Back")); // with its own way back
    // Going to a file takes everything along.
    w->showExtensionResult("fake", "The List", list);
    report.under = "list1";
    w->showExtensionResult("fake", "Claude's Report", report);
    QVERIFY(pane->isUnderShown());
    w->m_pane->view->setCurrentIndex(QModelIndex());
    w->m_pane->view->setCurrentIndex(w->toView(w->m_pane->model->indexForPath(a)));
    QTest::qWait(150);
    QVERIFY(pane->isUnderShown() && pane->resultId() == "list1"); // selecting a file leaves both where they are
    QTest::keyClick(w->m_pane->view, Qt::Key_Space); // asking for the file's preview takes the pane
    QTRY_VERIFY(pane->resultId().isEmpty());
    QVERIFY(!pane->isUnderShown() && pane->underText().isEmpty());
    w->hideQuickLook();
}

// What an extension shows (a dashboard, a list) keeps the preview pane while
// the user works with the files: selecting them and changing folder leave it
// alone. Space on a file asks for that file's preview and takes the pane;
// Space with nothing selected, Esc and the pane's ✕ close it.
void TestMainWindow::resultKeepsThePane()
{
    const QString a = make("a.txt", "the file's words");
    make("sub/inside.txt");
    open(m_dir);
    Extensions::PreviewReply content;
    content.kind = Extensions::PreviewReply::Text;
    content.text = "the dashboard";
    content.extension = "fake";
    content.resultId = "dash";
    w->showExtensionResult("Fake", "Dashboard", content);
    QVERIFY(w->quickLookVisible());
    QuickLook *pane = w->m_quickLook;
    QVERIFY(pane->hasResult());
    // Selecting a file, then another, then none.
    for (const QString &path : {a, m_dir + "/sub"}) {
        w->m_pane->view->setCurrentIndex(w->toView(w->m_pane->model->indexForPath(path)));
        QTest::qWait(60);
        QVERIFY(pane->hasResult());
        QCOMPARE(pane->shownText(), QStringLiteral("the dashboard"));
    }
    w->m_pane->view->setCurrentIndex(QModelIndex());
    QCOMPARE(pane->shownText(), QStringLiteral("the dashboard"));
    // Into another folder and back.
    open(m_dir + "/sub");
    QVERIFY(w->quickLookVisible() && pane->hasResult());
    QCOMPARE(pane->shownText(), QStringLiteral("the dashboard"));
    open(m_dir);
    QCOMPARE(pane->resultId(), QStringLiteral("dash"));
    // It is still kept up to date meanwhile.
    Extensions::PreviewReply again = content;
    again.update = true;
    again.text = "the dashboard, later";
    w->showExtensionResult("Fake", "Dashboard", again);
    QCOMPARE(pane->shownText(), QStringLiteral("the dashboard, later"));
    // Space on a file: that file's preview, and the pane follows the selection again.
    w->m_pane->view->setCurrentIndex(w->toView(w->m_pane->model->indexForPath(a)));
    QVERIFY(pane->hasResult());
    QTest::keyClick(w->m_pane->view, Qt::Key_Space);
    QVERIFY(w->quickLookVisible());
    QVERIFY(!pane->hasResult());
    QTRY_COMPARE(pane->path(), a);
    QTRY_COMPARE_WITH_TIMEOUT(pane->shownText(), QStringLiteral("the file's words"), 10000);
    w->m_pane->view->setCurrentIndex(w->toView(w->m_pane->model->indexForPath(m_dir + "/sub")));
    QTRY_COMPARE(pane->path(), m_dir + "/sub");
    // Space once more closes the pane, as it always did for a file's preview.
    QTest::keyClick(w->m_pane->view, Qt::Key_Space);
    QVERIFY(!w->quickLookVisible());

    // With nothing selected, Space over a result closes the pane; so does Esc.
    w->m_pane->view->setCurrentIndex(QModelIndex());
    w->showExtensionResult("Fake", "Dashboard", content);
    QVERIFY(w->quickLookVisible() && pane->hasResult());
    QTest::keyClick(w->m_pane->view, Qt::Key_Space);
    QVERIFY(!w->quickLookVisible());
    QVERIFY(!pane->hasResult());
    w->showExtensionResult("Fake", "Dashboard", content);
    QVERIFY(pane->hasResult());
    QTest::keyClick(w->m_pane->view, Qt::Key_Escape);
    QVERIFY(!w->quickLookVisible() && !pane->hasResult());
    // The line under its title says how to get a file's preview.
    w->showExtensionResult("Fake", "Dashboard", content);
    QVERIFY2(pane->detailsText().contains("Fake") && pane->detailsText().contains("Space previews the selected file"), qPrintable(pane->detailsText()));
    w->hideQuickLook();
}

// A result whose extension says it can be searched gets a field over it:
// what is typed there goes to the extension (which makes the result again),
// Esc empties it and then gives the keys back, and it goes with the result.
void TestMainWindow::extensionResultCanBeSearched()
{
    if (QStandardPaths::findExecutable("python3").isEmpty())
        QSKIP("python3 isn't installed (the test extension is a Python script)");
    Extensions::Host *host = Extensions::Host::instance();
    host->setEnabled("fake", true);
    const QString a = make("a.txt");
    open(m_dir);
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
    QTRY_VERIFY_WITH_TIMEOUT(w->m_quickLook && w->m_quickLook->resultId() == "live1", 10000);
    QLineEdit *field = w->m_quickLook->searchField();
    QVERIFY(field->isVisibleTo(w->m_quickLook));
    QCOMPARE(field->placeholderText(), QStringLiteral("Search the fake")); // its text is data: one line
    QVERIFY(!w->m_quickLook->hasOpenButton()); // a result is no file: nothing to open
    QTRY_VERIFY_WITH_TIMEOUT(w->m_quickLook->shownText().startsWith("live 2 for "), 10000);

    // Typing: told once the typing pauses, and the answer replaces the result where it is.
    QTest::keyClicks(field, "abc");
    QTRY_COMPARE_WITH_TIMEOUT(w->m_quickLook->shownText(), QStringLiteral("searched: abc"), 10000);
    QCOMPARE(w->m_quickLook->resultId(), QStringLiteral("live1"));
    QCOMPARE(field->text(), QStringLiteral("abc"));
    if (qEnvironmentVariableIsSet("ARGONAUT_TEST_GRAB")) { // to look at it
        QTest::qWait(200);
        w->grab().save(qEnvironmentVariable("ARGONAUT_TEST_GRAB"));
    }
    // Under it, what the extension offers to do with it: a button, and a line to type beside it.
    // They stay while the result is made again (the search just did that).
    {
        QuickLook *pane = w->m_quickLook;
        QVERIFY(pane->hasAction());
        QCOMPARE(pane->actionButton()->text(), QStringLiteral("Do It")); // its texts are data: one line
        QVERIFY(pane->actionInput()->isVisibleTo(pane));
        QCOMPARE(pane->actionInput()->placeholderText(), QStringLiteral("Say what"));
        QSignalSpy said(host, &Extensions::Host::message);
        auto heard = [&](const QString &part) {
            for (const QList<QVariant> &one : said) {
                if (one.at(1).toString().contains(part))
                    return true;
            }
            return false;
        };
        // The button with nothing typed, then with something; Enter in the field is the button too.
        pane->actionButton()->click();
        QTRY_VERIFY_WITH_TIMEOUT(!said.isEmpty() && said.last().at(1).toString() == "action act on live1:", 10000);
        QTRY_COMPARE_WITH_TIMEOUT(pane->shownText(), QStringLiteral("acted: "), 10000);
        QTest::keyClicks(pane->actionInput(), "  only the bus  ");
        pane->actionButton()->click();
        QTRY_VERIFY_WITH_TIMEOUT(heard("action act on live1: only the bus"), 10000);
        QTRY_COMPARE_WITH_TIMEOUT(pane->shownText(), QStringLiteral("acted: only the bus"), 10000);
        QVERIFY(pane->hasAction()); // made again: still there, with what was typed
        QCOMPARE(pane->actionInput()->text(), QStringLiteral("  only the bus  "));
        pane->actionInput()->clear();
        QSignalSpy keysBack(pane, &QuickLook::keysBackRequested);
        QTest::keyClicks(pane->actionInput(), "by enter");
        QTest::keyClick(pane->actionInput(), Qt::Key_Return);
        QTRY_VERIFY_WITH_TIMEOUT(heard("action act on live1: by enter"), 10000);
        QTest::keyClick(pane->actionInput(), Qt::Key_Escape); // the keys go back to the files
        QVERIFY(keysBack.size() >= 1);
    }
    // Asking for a file's preview (Space on it) takes the pane: the field goes with the result.
    w->m_pane->view->setCurrentIndex(QModelIndex());
    w->m_pane->view->setCurrentIndex(w->toView(w->m_pane->model->indexForPath(a)));
    QTest::keyClick(w->m_pane->view, Qt::Key_Space);
    QTRY_VERIFY(w->m_quickLook->resultId().isEmpty());
    QVERIFY(!w->m_quickLook->hasAction()); // and so does the button
    QVERIFY(w->m_quickLook->actionInput()->text().isEmpty());
    QVERIFY(w->m_quickLook->hasOpenButton()); // a file again: it can be opened
    QVERIFY(!field->isVisibleTo(w->m_quickLook));
    QVERIFY(field->text().isEmpty());

    // Again: Esc empties the field first, then hands the keys back.
    w->m_pane->view->setCurrentIndex(QModelIndex());
    QTRY_VERIFY_WITH_TIMEOUT(command("live"), 10000);
    QTRY_VERIFY_WITH_TIMEOUT(w->m_quickLook->resultId() == "live1" && field->isVisibleTo(w->m_quickLook), 10000);
    QTRY_VERIFY_WITH_TIMEOUT(w->m_quickLook->shownText().startsWith("live 2 for "), 10000);
    QSignalSpy back(w->m_quickLook, &QuickLook::keysBackRequested);
    QTest::keyClicks(field, "xy");
    QTRY_COMPARE_WITH_TIMEOUT(w->m_quickLook->shownText(), QStringLiteral("searched: xy"), 10000);
    QTest::keyClick(field, Qt::Key_Escape);
    QVERIFY(field->text().isEmpty());
    QCOMPARE(back.size(), 0);
    QTRY_COMPARE_WITH_TIMEOUT(w->m_quickLook->shownText(), QStringLiteral("searched: "), 10000);
    QTest::keyClick(field, Qt::Key_Escape);
    QCOMPARE(back.size(), 1);
    // Closing the pane takes the field along.
    w->hideQuickLook();
    QVERIFY(field->text().isEmpty());
    host->setEnabled("fake", false);
}

// A drive that isn't mounted is a row of the sidebar, as in Nautilus: a
// click mounts it (the desktop does, through gio: a script here) and opens
// it in this window; one that can't be mounted says so and nothing changes.
void TestMainWindow::unmountedDriveInTheSidebar()
{
    const QString tools = m_dir + "/pretend";
    QDir().mkpath(tools);
    const QString mountPoint = m_dir + "/media/PRETEND-ONE";
    QVERIFY(TestSupport::writeScript(tools + "/gio", QStringLiteral(R"(#!/bin/sh
if [ "$1 $2" = "mount -li" ]; then
printf 'Drive(0): Disk\n  ids:\n   unix-device: \047/dev/argonaut-test\047\n  is_removable=0\n'
printf '  Volume(0): PRETEND-ONE\n    ids:\n     unix-device: \047/dev/argonaut-test1\047\n    can_mount=1\n'
printf '  Volume(1): PRETEND-TWO\n    ids:\n     unix-device: \047/dev/argonaut-test2\047\n    can_mount=1\n'
exit 0
fi
if [ "$1 $2 $3" = "mount -d /dev/argonaut-test1" ]; then mkdir -p '%1'; echo "Mounted /dev/argonaut-test1 at %1"; exit 0; fi
echo "gio: $3: Not authorized to perform operation" >&2
exit 2
)").arg(mountPoint)));
    const QByteArray gioBefore = qgetenv("ARGONAUT_GIO_PROGRAM");
    qputenv("ARGONAUT_GIO_PROGRAM", QFile::encodeName(tools + "/gio"));
    qputenv("ARGONAUT_VOLUMES", "1");
    const auto restore = qScopeGuard([gioBefore] {
        qputenv("ARGONAUT_GIO_PROGRAM", gioBefore);
        qunsetenv("ARGONAUT_VOLUMES");
    });
    open(m_dir);
    auto row = [this](const QString &text) -> QListWidgetItem * {
        for (int i = 0; i < w->m_sidebar->count(); ++i) {
            if (w->m_sidebar->item(i)->text() == text)
                return w->m_sidebar->item(i);
        }
        return nullptr;
    };
    w->m_sidebar->refresh();
    QTRY_VERIFY_WITH_TIMEOUT(row("PRETEND-ONE") && row("PRETEND-TWO"), 10000);
    QCOMPARE(Sidebar::pathOf(row("PRETEND-ONE")), QStringLiteral("volume:///dev/argonaut-test1"));
    QCOMPARE(Sidebar::kindOf(row("PRETEND-ONE")), Sidebar::DriveItem);
    QVERIFY(!Sidebar::canEject(row("PRETEND-ONE"))); // nothing to eject yet
    QVERIFY(row("PRETEND-ONE")->toolTip().contains("Not mounted"));

    // One that the desktop refuses: said, and the window stays where it is.
    emit w->m_sidebar->itemClicked(row("PRETEND-TWO"));
    QTRY_VERIFY_WITH_TIMEOUT(w->m_pane->message.contains("could not be mounted"), 10000);
    QVERIFY2(w->m_pane->message.contains("PRETEND-TWO") && w->m_pane->message.contains("Not authorized"), qPrintable(w->m_pane->message));
    QCOMPARE(w->m_pane->current, m_dir);

    // A click on the other: mounted, and opened here.
    emit w->m_sidebar->itemClicked(row("PRETEND-ONE"));
    QTRY_COMPARE_WITH_TIMEOUT(w->m_pane->current, mountPoint, 10000);
    QVERIFY(QFileInfo(mountPoint).isDir());
}

// Changes an extension proposes are shown in the preview pane: a tick a
// move, what can't be done unticked with the reason. The plan keeps the pane
// while files are looked at. Apply checks again (the folder may have
// changed), does what still can be done through the file engine, and it
// can be undone; the extension only hears how it went.
void TestMainWindow::extensionPlanInThePane()
{
    if (QStandardPaths::findExecutable("python3").isEmpty())
        QSKIP("python3 isn't installed (the test extension is a Python script)");
    Extensions::Host *host = Extensions::Host::instance();
    host->setEnabled("fake", true);
    const QString a = make("a.txt"), b = make("b.txt"), c = make("c.txt"), trigger = make("make-plan.txt");
    make("d.txt");
    make("e.txt");
    open(m_dir);
    QSignalSpy said(host, &Extensions::Host::message);
    auto heard = [&](const QString &part) {
        for (const QList<QVariant> &one : said) {
            if (one.at(1).toString().contains(part))
                return true;
        }
        return false;
    };
    auto propose = [&] {
        const QList<Extensions::Item> one{{trigger, "text/plain", false}};
        for (const Extensions::Command &command : host->commands(m_dir, one)) {
            if (command.id == "hello") {
                host->invoke(command, m_dir, {trigger});
                return true;
            }
        }
        return false;
    };
    QTRY_VERIFY_WITH_TIMEOUT(propose(), 10000);
    QTRY_VERIFY_WITH_TIMEOUT(w->m_quickLook && w->m_quickLook->hasPlan(), 10000);
    QVERIFY(w->quickLookVisible());
    QuickLook *pane = w->m_quickLook;
    PlanView *plan = pane->planView();
    QCOMPARE(plan->moveCount(), 7);
    QVERIFY(!pane->hasOpenButton());
    // What can be done is ticked; what can't is said why, and can't be ticked.
    QVERIFY(plan->moveText(0).startsWith("a.txt") && plan->moveText(0).contains("→ Texts/") && plan->moveText(0).contains("A text"));
    QVERIFY(plan->isMoveTicked(0) && plan->isMoveTicked(1) && plan->isMoveTicked(2));
    QVERIFY2(plan->moveText(2).contains("renamed.txt"), qPrintable(plan->moveText(2)));
    for (int row = 3; row < 7; ++row) {
        QVERIFY2(!plan->canMoveBeTicked(row) && !plan->isMoveTicked(row), qPrintable(plan->moveText(row)));
        QVERIFY2(plan->moveText(row).contains("Can't be done"), qPrintable(plan->moveText(row)));
        plan->setMoveTicked(row, true); // not even by asking
        QVERIFY(!plan->isMoveTicked(row));
    }
    QCOMPARE(int(plan->keptMoves().size()), 3);
    if (qEnvironmentVariableIsSet("ARGONAUT_TEST_GRAB")) { // to look at it
        QTest::qWait(300);
        w->grab().save(qEnvironmentVariable("ARGONAUT_TEST_GRAB"));
    }

    // Looking at a file meanwhile doesn't take the plan away, nor does another result.
    w->m_pane->view->setCurrentIndex(w->toView(w->m_pane->model->indexForPath(a)));
    QTest::qWait(200);
    QVERIFY(pane->hasPlan());
    Extensions::PreviewReply other;
    other.kind = Extensions::PreviewReply::Text;
    other.text = "something else";
    w->showExtensionResult("fake", "Other", other);
    QVERIFY(pane->hasPlan());

    // Dismiss: nothing moves, and the extension is told.
    plan->dismissButton()->click();
    QVERIFY(!pane->hasPlan());
    QTRY_VERIFY_WITH_TIMEOUT(heard("plan p1 dismissed"), 10000);
    QVERIFY(QFile::exists(a) && QFile::exists(b) && QFile::exists(c) && !QFileInfo::exists(m_dir + "/Texts"));

    // Again; this time one move is unticked, and a file goes away before Apply.
    said.clear();
    QTRY_VERIFY_WITH_TIMEOUT(propose(), 10000);
    QTRY_VERIFY_WITH_TIMEOUT(pane->hasPlan(), 10000);
    plan->setMoveTicked(2, false); // c.txt keeps its name
    QCOMPARE(int(plan->keptMoves().size()), 2);
    QVERIFY(QFile::remove(b)); // by another program, meanwhile
    runJob([&] { plan->applyButton()->click(); });
    QVERIFY(!pane->hasPlan());
    QVERIFY(QFile::exists(m_dir + "/Texts/a.txt") && !QFile::exists(a)); // moved
    QVERIFY(QFile::exists(c) && !QFile::exists(m_dir + "/renamed.txt")); // unticked: untouched
    QVERIFY(!QFileInfo::exists(m_dir + "/Texts/Old"));                    // its file was gone: nothing made for it
    QTRY_VERIFY_WITH_TIMEOUT(heard("plan p1 applied=1 skipped=1"), 10000);
    // It went through the file engine: undo puts the file back.
    QVERIFY(w->m_undoAct->isEnabled());
    runJob([&] { w->m_undoAct->trigger(); });
    QVERIFY(QFile::exists(a) && !QFile::exists(m_dir + "/Texts/a.txt"));

    // Closing the pane with a plan in it dismisses the plan.
    said.clear();
    QTRY_VERIFY_WITH_TIMEOUT(propose(), 10000);
    QTRY_VERIFY_WITH_TIMEOUT(pane->hasPlan(), 10000);
    w->hideQuickLook();
    QTRY_VERIFY_WITH_TIMEOUT(heard("plan p1 dismissed"), 10000);
    QVERIFY(QFile::exists(a));
    host->setEnabled("fake", false);
}

// While Claude is asked something for an extension, the preview pane says
// so (with how long it has been), and keeps saying it while files are
// looked at; it ends with the answer, or when the user stops waiting.
void TestMainWindow::waitingForClaudeIsShown()
{
    const QString a = make("a.txt");
    open(m_dir);
    QVERIFY(!w->quickLookVisible());
    w->showClaudeWaiting("some-extension", "Some Extension", 1);
    QVERIFY(w->quickLookVisible());
    QuickLook *pane = w->m_quickLook;
    QVERIFY(pane->isWorking());
    QVERIFY2(pane->workingText().startsWith("Waiting for Claude's answer."), qPrintable(pane->workingText()));
    QVERIFY(!pane->hasOpenButton());
    QVERIFY(pane->stopWaitingButton()->isVisibleTo(pane));
    // It is seen to go on: after a while it says how long it has been.
    QTRY_VERIFY_WITH_TIMEOUT(pane->workingText().contains("seconds so far"), 6000);
    // A file selected meanwhile doesn't take it away.
    w->m_pane->view->setCurrentIndex(w->toView(w->m_pane->model->indexForPath(a)));
    QTest::qWait(200);
    QVERIFY(pane->isWorking());
    // Another extension's end isn't this one's.
    w->showClaudeWaiting("another", "Another", 0);
    QVERIFY(pane->isWorking());
    // "Stop Waiting" asks for that.
    QSignalSpy stop(pane, &QuickLook::workingStopRequested);
    pane->stopWaitingButton()->click();
    QCOMPARE(stop.size(), 1);
    QCOMPARE(stop.first().at(0).toString(), QStringLiteral("some-extension"));
    // The answer is there: the pane is free again, and follows the files.
    w->showClaudeWaiting("some-extension", "Some Extension", 0);
    QVERIFY(!pane->isWorking());
    QVERIFY(!pane->stopWaitingButton()->isVisibleTo(pane));
    w->m_pane->view->setCurrentIndex(QModelIndex());
    w->m_pane->view->setCurrentIndex(w->toView(w->m_pane->model->indexForPath(a)));
    QTRY_VERIFY(pane->hasOpenButton());

    // A plan that arrives while it is waiting takes the pane (it is what was waited for).
    w->showClaudeWaiting("some-extension", "Some Extension", 1);
    QVERIFY(pane->isWorking());
    Extensions::Plan plan;
    plan.extension = "some-extension";
    plan.extensionName = "Some Extension";
    plan.id = "p";
    plan.folder = m_dir;
    plan.moves = {{"a.txt", "Texts", {}, "A text", {}, {}}};
    QVERIFY(pane->showPlan(plan));
    QVERIFY(pane->hasPlan() && !pane->isWorking());
    w->hideQuickLook();

    // A plan's steps of every kind, as the checklist says them: what becomes of each is plain to read.
    Extensions::Plan steps = plan;
    steps.id = "q";
    auto step = [](const char *action, const char *item, const char *folder, const char *newName, const char *problem = "") {
        Extensions::PlanMove move;
        move.action = action;
        move.item = item;
        move.folder = folder;
        move.newName = newName;
        move.problem = problem;
        return move;
    };
    steps.moves = {step("trash", "a.txt", "", ""), step("copy", "a.txt", "Copies", ""), step("compress", "a.txt", "", "Texts.zip"),
                   step("folder", "", "New/Deeper", ""), step("move", "a.txt", "", "b.txt"), step("trash", "/etc/passwd", "", "", "isn't in this folder")};
    w->toggleQuickLook();
    QVERIFY(pane->showPlan(steps));
    PlanView *list = pane->planView();
    QCOMPARE(list->moveCount(), 6);
    QVERIFY2(list->moveText(0).startsWith("a.txt") && list->moveText(0).contains("→ Trash"), qPrintable(list->moveText(0)));
    QVERIFY2(list->moveText(1).contains("→ a copy: Copies/"), qPrintable(list->moveText(1)));
    QVERIFY2(list->moveText(2).contains("→ into Texts.zip"), qPrintable(list->moveText(2)));
    QVERIFY2(list->moveText(3).startsWith("New folder") && list->moveText(3).contains("→ New/Deeper/"), qPrintable(list->moveText(3)));
    QVERIFY2(list->moveText(4).contains("→ b.txt"), qPrintable(list->moveText(4)));
    QVERIFY(list->isMoveTicked(0) && !list->canMoveBeTicked(5) && !list->isMoveTicked(5)); // what is refused can't be ticked
    w->hideQuickLook();
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
        // In the command palette as an extension's: "ext::…", with the menu
        // and the extension's name where it lives.
        w->showCommandPalette();
        bool inPalette = false;
        for (const CommandPalette::Command &c : w->m_palette->commands()) {
            QVERIFY2(c.name != "In The & Menu", "an extension's command without its ext:: name");
            if (c.name == "ext::In The & Menu") {
                inPalette = true;
                QVERIFY2(c.where.startsWith("View · "), qPrintable(c.where));
            }
        }
        w->m_palette->close();
        QVERIFY(inPalette);
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

    // Selecting a file doesn't take the pane from it: it goes on being kept up to date.
    QTRY_VERIFY(w->m_pane->model->indexForPath(a).isValid());
    w->m_pane->view->setCurrentIndex(QModelIndex());
    w->m_pane->view->setCurrentIndex(w->toView(w->m_pane->model->indexForPath(a)));
    QTest::qWait(150);
    QCOMPARE(w->m_quickLook->resultId(), QStringLiteral("live1"));
    QCOMPARE(logged(), QStringLiteral("live1 shown\n"));
    // Space on the file does: the extension is told, and its next version is dropped.
    QTest::keyClick(w->m_pane->view, Qt::Key_Space);
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

// Results are kept in tabs of the preview pane: a second one doesn't
// replace the first, only the chosen tab's extension hears it is shown,
// and a tab goes when it is closed (or the pane is).
void TestMainWindow::resultsStayInTabs()
{
    if (QStandardPaths::findExecutable("python3").isEmpty())
        QSKIP("python3 isn't installed (the test extension is a Python script)");
    Extensions::Host *host = Extensions::Host::instance();
    host->setEnabled("fake", true);
    // Off again however the test ends, or the next tests meet it.
    const QScopeGuard off([host] { host->setEnabled("fake", false); });
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
    QTRY_VERIFY_WITH_TIMEOUT(w->quickLookVisible(), 10000);
    QuickLook *ql = w->m_quickLook;
    QTRY_VERIFY_WITH_TIMEOUT(ql->shownText().startsWith("live 2 for "), 10000);
    QCOMPARE(ql->resultTabCount(), 1);
    QVERIFY(ql->resultTabs()->isVisible());
    QCOMPARE(ql->resultTabs()->count(), 2); // Preview, Live
    QCOMPARE(ql->resultTabs()->currentIndex(), 1);
    QCOMPARE(ql->resultTabs()->tabText(1), QStringLiteral("Live"));
    // What was typed in its search field is kept with the tab.
    QVERIFY(ql->searchField()->isVisible());
    ql->searchField()->setText("needle");

    // A second result: a tab of its own, chosen; the first is kept, hidden.
    QVERIFY(command("showme"));
    QTRY_COMPARE_WITH_TIMEOUT(ql->resultTabCount(), 2, 10000);
    QCOMPARE(ql->resultTabs()->currentIndex(), 2);
    QVERIFY(ql->resultId().isEmpty()); // the diff has no name
    QVERIFY(!ql->searchField()->isVisible());
    QTRY_COMPARE_WITH_TIMEOUT(logged(), QStringLiteral("live1 shown\nlive1 hidden\n"), 10000);
    // The hidden one's extension isn't stopped as idle meanwhile.
    QVERIFY(host->isRunning("fake"));

    // Back to the first: shown again (its extension makes it anew), with its search.
    ql->resultTabs()->setCurrentIndex(1);
    QCOMPARE(ql->resultId(), QStringLiteral("live1"));
    QCOMPARE(ql->searchField()->text(), QStringLiteral("needle"));
    QTRY_COMPARE_WITH_TIMEOUT(logged(), QStringLiteral("live1 shown\nlive1 hidden\nlive1 shown\n"), 10000);
    // The extension forgot the search when the tab was left: it is told again.
    QTRY_COMPARE_WITH_TIMEOUT(ql->shownText(), QStringLiteral("searched: needle"), 10000);

    // Space on a file: the Preview tab; the results stay in theirs.
    QTRY_VERIFY(w->m_pane->model->indexForPath(a).isValid());
    w->m_pane->view->setCurrentIndex(w->toView(w->m_pane->model->indexForPath(a)));
    QTest::keyClick(w->m_pane->view, Qt::Key_Space);
    QTRY_COMPARE(ql->path(), a);
    QCOMPARE(ql->resultTabs()->currentIndex(), 0);
    QCOMPARE(ql->resultTabCount(), 2);
    QVERIFY(!ql->hasResult());
    // The Live tab again, then closed: the diff's comes forward.
    ql->resultTabs()->setCurrentIndex(1);
    QCOMPARE(ql->resultId(), QStringLiteral("live1"));
    ql->closeResultTab(0);
    QCOMPARE(ql->resultTabCount(), 1);
    QCOMPARE(ql->resultTabs()->currentIndex(), 1);
    QVERIFY(ql->hasResult());
    QVERIFY(ql->resultId().isEmpty());
    QTRY_VERIFY_WITH_TIMEOUT(logged().endsWith("live1 shown\nlive1 hidden\n"), 10000);
    // The last one closed: the bar goes and the selected file is previewed.
    ql->closeResultTab(0);
    QCOMPARE(ql->resultTabCount(), 0);
    QVERIFY(!ql->resultTabs()->isVisible());
    QVERIFY(!ql->hasResult());
    QTRY_COMPARE(ql->path(), a);

    // Closing the pane takes every tab with it.
    QVERIFY(command("live"));
    QTRY_COMPARE_WITH_TIMEOUT(ql->resultTabCount(), 1, 10000);
    w->hideQuickLook();
    QCOMPARE(ql->resultTabCount(), 0);
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
    QVERIFY(w->m_quickLook->hasResult());
    // It stays while files are selected: it is being read. Space on a file takes the pane back.
    w->m_pane->view->setCurrentIndex(w->toView(w->m_pane->model->indexForPath(a)));
    QTest::qWait(150);
    QVERIFY(w->m_quickLook->hasResult() && w->m_quickLook->path().isEmpty());
    QVERIFY(w->m_quickLook->shownText().contains("+new line"));
    QTest::keyClick(w->m_pane->view, Qt::Key_Space);
    QTRY_COMPARE(w->m_quickLook->path(), a);
    QVERIFY(!w->m_quickLook->hasResult() && w->quickLookVisible());

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
