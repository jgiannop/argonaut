// Tests for the window's own decisions about what gets trashed, deleted,
// moved or returned: the code between the user and the file engine (which
// tst_fileoperations covers). They drive the real MainWindow, headless.
//
// Everything is sandboxed: HOME, the XDG folders and the trash live in a
// temporary folder, real drives' trash folders are never looked at
// (ARGONAUT_TRASH_HOME_ONLY), and `gio` / the eject program are fake scripts
// that only write down how they were called.

#include "DirectoryModel.h"
#include "FileOperations.h"
#include "MainWindow.h"
#include "Trash.h"

#if defined(ARGONAUT_DBUS)
#include "ChooserBar.h"
#include "FileChooserPortal.h"
#include "FileChooserRequest.h"
#endif

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMessageBox>
#include <QPointer>
#include <QSignalSpy>
#include <QTemporaryDir>
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
    void readOnlyFolderOnlyAllowsCopy();
    void cutIsPastedOnce();
    void dropRules();
    void dropOnTheTrashTrashes();
    void splitViewActsOnThePaneYouAreIn();
    void showHiddenAppliesToBothPanes();
    void tabsOfPlacesHaveTheirNames();
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
    QSignalSpy loaded(w->m_model, &DirectoryModel::loaded);
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
    QVERIFY(w->m_model->isTrash());
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
    QCOMPARE(w->tabTitle(DirectoryModel::recentPath()), w->titleFor(DirectoryModel::recentPath()));
    QCOMPARE(w->tabTitle(DirectoryModel::starredPath()), w->titleFor(DirectoryModel::starredPath()));
    QCOMPARE(w->tabTitle(DirectoryModel::recentPath()), QStringLiteral("Recent"));
    QDir().mkpath(Trash::homeFilesDir());
    QCOMPARE(w->tabTitle(Trash::homeFilesDir()), QStringLiteral("Trash"));
    QCOMPARE(w->tabTitle(m_dir), QFileInfo(m_dir).fileName());
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
    QSignalSpy loaded(w->m_model, &DirectoryModel::loaded);
    w->startChooser(request);
    QTRY_VERIFY_WITH_TIMEOUT(!loaded.isEmpty(), 10000);
    QCOMPARE(w->m_current, m_dir);

    // Nothing chosen: no answer yet.
    w->chooserAccept();
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
    w->chooserAccept();
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
    QSignalSpy loaded(w->m_model, &DirectoryModel::loaded);
    w->startChooser(request);
    QTRY_VERIFY_WITH_TIMEOUT(!loaded.isEmpty(), 10000);
    QCOMPARE(w->m_chooserBar->name(), QStringLiteral("report.txt"));

    answerNext("Cancel");
    w->chooserAccept();
    QCOMPARE(m_asked, 1);
    QVERIFY(m_question.contains("report.txt"));
    QCOMPARE(finished.count(), 0); // still open, nothing answered

    // A name that is free needs no question.
    w->m_chooserBar->setName("new.txt");
    w->chooserAccept();
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
    w->chooserAccept();
    w->cancelChooser();
    QCOMPARE(finished.count(), 1);
}

#endif

int main(int argc, char **argv)
{
    QTemporaryDir sandbox;
    if (!sandbox.isValid())
        return 1;
    // Must happen before QApplication / QStandardPaths / QSettings are used.
    const QString root = sandbox.path();
    qputenv("HOME", QFile::encodeName(root + "/home"));
    qputenv("XDG_DATA_HOME", QFile::encodeName(root + "/data"));
    qputenv("XDG_CONFIG_HOME", QFile::encodeName(root + "/config"));
    qputenv("XDG_CACHE_HOME", QFile::encodeName(root + "/cache"));
    // Never look at (or empty) the trash folders of real mounted drives.
    qputenv("ARGONAUT_TRASH_HOME_ONLY", "1");
    qputenv("QT_QPA_PLATFORM", "offscreen");
    for (const char *name : {"/home", "/data", "/config", "/cache"})
        QDir().mkpath(root + name);
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
    qputenv("ARGONAUT_KEYSTORE", "file"); // never the real keyring
    qputenv("ARGONAUT_NATIVE_DECORATIONS", "1");

    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName("ArgonautTest");
    QCoreApplication::setApplicationName("ArgonautTest");
    TestMainWindow test(root);
    return QTest::qExec(&test, argc, argv);
}

#include "tst_mainwindow.moc"
