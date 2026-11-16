// Tests for the terminal interface (Tui::App): what is on screen, what the
// keys do, and above all that nothing permanent happens without a "y".
// The interface draws into a grid of cells, so no terminal is needed.

#include "App.h"
#include "Shell.h"
#include "FileOperations.h"
#include "Screen.h"
#include "TestSupport.h"
#include "Trash.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QSettings>
#include <QSignalSpy>
#include <QtTest>

#include <memory>
#include <unistd.h>

using Tui::App;
using Tui::Key;
using Tui::Screen;

class TestTui : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void screenCells();
    void showsTwoPanes();
    void movesAndOpens();
    void selects();
    void copiesAndMovesToTheOtherPane();
    void trashesAndUndoes();
    void deletingForeverAsksFirst();
    void deleteInTheTrashAsksFirst();
    void readOnlyFolderIsLeftAlone();
    void newFolderAndRename();
    void nameClashIsAQuestion();
    void hiddenFilesAndGoTo();
    void quitWaitsForTheWork();
    void mouseClicksAndDoubleClicks();
    void opensFiles();
    void startsWithOnePane();
    void gridView();
    void bigIcons();
    void shellInTheFolder();
    void shellPanel();

private:
    QString make(const QString &relative, const QByteArray &content = "x");
    bool exists(const QString &path) const { return QFileInfo::exists(path); }
    void settle();               // folders listed, operations done
    QString screen(int width = 100, int height = 24);
    void keys(std::initializer_list<Key> list);
    void type(const QString &text);
    void runJob(const std::function<void()> &action);

    QTemporaryDir *m_tmp = nullptr;
    QString m_left, m_right;
    std::unique_ptr<App> app;
};

QString TestTui::make(const QString &relative, const QByteArray &content)
{
    const QString path = m_left + "/" + relative;
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (f.open(QIODevice::WriteOnly))
        f.write(content);
    return path;
}

void TestTui::settle()
{
    QTRY_VERIFY_WITH_TIMEOUT(!app->isLoading() && !app->isBusy(), 15000);
    QTest::qWait(60); // a rescan asked for by the last change
    QTRY_VERIFY_WITH_TIMEOUT(!app->isLoading(), 15000);
}

QString TestTui::screen(int width, int height)
{
    Screen s(width, height);
    app->render(s);
    return s.text();
}

void TestTui::keys(std::initializer_list<Key> list)
{
    for (const Key &key : list)
        app->key(key);
}

void TestTui::type(const QString &text)
{
    for (uint c : text.toUcs4())
        app->key(Key::character(c));
}

void TestTui::runJob(const std::function<void()> &action)
{
    QSignalSpy finished(app->operations(), &FileOperations::jobFinished);
    action();
    QTRY_VERIFY_WITH_TIMEOUT(!finished.isEmpty(), 15000);
    settle();
}

void TestTui::init()
{
    m_tmp = new QTemporaryDir(QDir::homePath() + "/tui-XXXXXX"); // on the home drive: it has a Trash
    QVERIFY(m_tmp->isValid());
    m_left = QDir::cleanPath(m_tmp->path()) + "/left";
    m_right = QDir::cleanPath(m_tmp->path()) + "/right";
    QDir().mkpath(m_left);
    QDir().mkpath(m_right);
    make("Documents/inside.txt");
    make("alpha.txt", "alpha");
    make("beta.txt", "beta!");
    make(".secret");
    QDir(Trash::homeFilesDir()).removeRecursively();
    QDir(QFileInfo(Trash::homeFilesDir()).absolutePath() + "/info").removeRecursively();
    QSettings().remove("tui"); // every test starts with lists
    app = std::make_unique<App>(m_left, m_right);
    settle();
}

void TestTui::cleanup()
{
    app.reset();
    delete m_tmp;
}

// The grid: wide characters take two cells, control characters never reach
// the terminal, long names are cut in the middle.
void TestTui::screenCells()
{
    Screen s(20, 2);
    QCOMPARE(s.put(0, 0, QStringLiteral("ab日本c")), 7);
    QCOMPARE(s.line(0), QStringLiteral("ab日本c"));
    QVERIFY(s.at(3, 0).tail);
    QCOMPARE(s.put(0, 1, QStringLiteral("a\x1b[31mb\nc")), 9);
    QCOMPARE(s.line(1), QStringLiteral("a?[31mb?c")); // an escape sequence in a name is only text
    QCOMPARE(s.put(18, 0, QStringLiteral("xyz")), 2);   // cut at the edge
    QCOMPARE(Screen::elided(QStringLiteral("a-very-long-name.tar.gz"), 12), QStringLiteral("a-very…ar.gz")); // the end stays: the extension
    QCOMPARE(Screen::elided(QStringLiteral("a-very-long-name"), 8, false), QStringLiteral("a-very-…"));
    const int wide = Screen::widthOf(Screen::elided(QStringLiteral("日本語のファイル名.txt"), 11));
    QVERIFY(wide <= 11 && wide >= 9); // never wider than asked; a wide character can leave a cell over
    QCOMPARE(Screen::elided(QStringLiteral("short"), 10), QStringLiteral("short"));
}

void TestTui::showsTwoPanes()
{
    const QString text = screen();
    const QStringList lines = text.split('\n');
    QVERIFY(lines.at(0).contains("Argonaut"));
    // Both folders in the headings, folders first and marked, hidden files not shown.
    QVERIFY2(lines.at(1).contains("left") && lines.at(1).contains("right"), qPrintable(lines.at(1)));
    QVERIFY(lines.at(2).contains("Name") && lines.at(2).contains("Size"));
    QVERIFY2(lines.at(3).contains("../") && lines.at(3).contains("Up"), qPrintable(lines.at(3))); // the way up
    QVERIFY(lines.at(4).contains("Documents/"));
    QVERIFY(lines.at(5).contains("alpha.txt") && lines.at(5).contains("5 bytes"));
    QVERIFY(lines.at(6).contains("beta.txt"));
    QVERIFY(!text.contains(".secret"));
    QVERIFY(text.contains("3 items"));
    QVERIFY(text.contains("Nothing here")); // the empty right pane
    QVERIFY(lines.last().contains("F5 Copy") && lines.last().contains("F10 Quit"));
    // The cursor row of the active pane stands out.
    Screen s(100, 24);
    app->render(s);
    QCOMPARE(s.at(3, 4).style.paper, Tui::Paper::Cursor); // on the first item, not on ".."
    QCOMPARE(s.at(3, 3).style.paper, Tui::Paper::Normal);
    QCOMPARE(s.at(3, 5).style.paper, Tui::Paper::Normal);
    // A terminal that is too small says so instead of drawing rubbish.
    QVERIFY(screen(30, 5).contains("too small"));
    // F1: the keys.
    keys({Key::of(Key::F1)});
    QVERIFY(screen(100, 40).contains("copy / move to the other pane"));
    keys({Key::character('x')}); // any key closes it, and does nothing else
    QVERIFY(screen().contains("alpha.txt"));
}

void TestTui::movesAndOpens()
{
    QCOMPARE(app->cursorPath(), m_left + "/Documents");
    keys({Key::of(Key::Down)});
    QCOMPARE(app->cursorPath(), m_left + "/alpha.txt");
    keys({Key::of(Key::End)});
    QCOMPARE(app->cursorPath(), m_left + "/beta.txt");
    keys({Key::of(Key::Down)}); // not past the end
    QCOMPARE(app->cursorPath(), m_left + "/beta.txt");
    // Home is the ".." row: not an item, and Enter on it goes up.
    keys({Key::of(Key::Home)});
    QVERIFY(app->cursorPath().isEmpty());
    keys({Key::of(Key::Enter)});
    QCOMPARE(app->folder(0), QFileInfo(m_left).path());
    settle();
    QCOMPARE(app->cursorPath(), m_left); // on the folder we came out of
    keys({Key::of(Key::Enter)});
    QCOMPARE(app->folder(0), m_left);
    settle();
    keys({Key::of(Key::Home), Key::of(Key::Down), Key::of(Key::Enter)});
    QCOMPARE(app->folder(0), m_left + "/Documents");
    settle();
    QCOMPARE(app->cursorPath(), m_left + "/Documents/inside.txt");
    // Up again: the cursor is on the folder we came out of.
    keys({Key::of(Key::Backspace)});
    settle();
    QCOMPARE(app->folder(0), m_left);
    QCOMPARE(app->cursorPath(), m_left + "/Documents");
    // Tab: the other pane takes the keys.
    keys({Key::of(Key::Tab)});
    QCOMPARE(app->activePane(), 1);
    QVERIFY(app->cursorPath().isEmpty());
    keys({Key::of(Key::Tab)});
    QCOMPARE(app->activePane(), 0);
    // Back (Alt+Left) after going somewhere.
    keys({Key::of(Key::Enter)});
    settle();
    Key back = Key::of(Key::Left);
    back.alt = true;
    keys({back});
    QCOMPARE(app->folder(0), m_left);
}

void TestTui::selects()
{
    keys({Key::of(Key::Down), Key::character(' ')}); // alpha, and the cursor steps down
    QCOMPARE(app->selectedPaths(), QStringList{m_left + "/alpha.txt"});
    QCOMPARE(app->cursorPath(), m_left + "/beta.txt");
    QVERIFY(screen().contains("* alpha.txt"));
    QVERIFY(screen().contains("1 selected (5 bytes)"));
    keys({Key::of(Key::Insert)});
    QCOMPARE(app->selectedPaths().size(), 2);
    keys({Key::of(Key::Up), Key::character(' ')}); // again: off
    QCOMPARE(app->selectedPaths(), QStringList{m_left + "/beta.txt"});
    keys({Key::control('a')});
    QCOMPARE(app->selectedPaths().size(), 3);
    keys({Key::of(Key::Escape)});
    QVERIFY(app->selectedPaths().isEmpty());
}

void TestTui::copiesAndMovesToTheOtherPane()
{
    // F5 with nothing selected: the item under the cursor.
    keys({Key::of(Key::Down)});
    runJob([&] { keys({Key::of(Key::F5)}); });
    QVERIFY(exists(m_right + "/alpha.txt") && exists(m_left + "/alpha.txt"));
    QVERIFY2(app->message().startsWith("Copied 1 item"), qPrintable(app->message()));
    QVERIFY(screen().contains("Ctrl+Z Undo Copy"));
    // F6 with a selection: those, wherever the cursor is.
    app->selectPath(m_left + "/beta.txt");
    keys({Key::character(' '), Key::of(Key::Home)});
    runJob([&] { keys({Key::of(Key::F6)}); });
    QVERIFY(exists(m_right + "/beta.txt") && !exists(m_left + "/beta.txt"));
    QVERIFY(exists(m_left + "/Documents")); // the cursor's item wasn't touched
    QVERIFY(app->selectedPaths().isEmpty());
    // Undo the move: it is back.
    runJob([&] { keys({Key::control('z')}); });
    QVERIFY(exists(m_left + "/beta.txt") && !exists(m_right + "/beta.txt"));
    QCOMPARE(app->message(), QStringLiteral("Undone"));
    runJob([&] { keys({Key::control('y')}); });
    QVERIFY(!exists(m_left + "/beta.txt") && exists(m_right + "/beta.txt"));
    // Moving something to where it already is: said, not done.
    keys({Key::of(Key::Tab)});
    settle();
    app->selectPath(m_right + "/beta.txt");
    keys({Key::of(Key::Tab), Key::control('l'), Key::control('u')});
    type(m_right);
    keys({Key::of(Key::Enter)});
    settle();
    app->selectPath(m_right + "/beta.txt");
    keys({Key::of(Key::F6)});
    QCOMPARE(app->message(), QStringLiteral("They're already in that folder"));
    QVERIFY(!app->isBusy());
}

void TestTui::trashesAndUndoes()
{
    app->selectPath(m_left + "/alpha.txt");
    runJob([&] { keys({Key::of(Key::F8)}); });
    QVERIFY(!exists(m_left + "/alpha.txt"));
    QVERIFY(exists(Trash::homeFilesDir() + "/alpha.txt")); // in the Trash, not gone
    QVERIFY(exists(m_left + "/beta.txt"));
    QVERIFY(app->message().contains("Ctrl+Z brings them back"));
    QVERIFY(!app->isAsking()); // the Trash can be undone: no question
    runJob([&] { keys({Key::control('z')}); });
    QVERIFY(exists(m_left + "/alpha.txt"));
    QVERIFY(!exists(Trash::homeFilesDir() + "/alpha.txt"));
}

void TestTui::deletingForeverAsksFirst()
{
    app->selectPath(m_left + "/alpha.txt");
    for (const Key &startKey : {Key::character('D'), [] { Key k = Key::of(Key::Delete); k.shift = true; return k; }()}) {
        keys({startKey});
        QVERIFY(app->isAsking());
        QVERIFY2(screen().contains("Permanently delete “alpha.txt”?"), qPrintable(screen()));
        QVERIFY(screen().contains("y: Delete"));
        // Enter, Esc, "n", a slip of the finger: nothing happens.
        keys({Key::of(Key::Enter)});
        QVERIFY(!app->isAsking());
        QTest::qWait(150);
        QVERIFY(exists(m_left + "/alpha.txt"));
        QVERIFY(!app->isBusy());
        QCOMPARE(app->message(), QStringLiteral("Nothing was changed"));
    }
    for (const Key &no : {Key::of(Key::Escape), Key::character('n'), Key::character('d'), Key::of(Key::F8)}) {
        keys({Key::character('D'), no});
        QTest::qWait(50);
        QVERIFY(exists(m_left + "/alpha.txt"));
        QVERIFY(!app->isAsking());
    }
    // Only "y" deletes, and only what was asked about.
    runJob([&] { keys({Key::character('D'), Key::character('y')}); });
    QVERIFY(!exists(m_left + "/alpha.txt"));
    QVERIFY(!exists(Trash::homeFilesDir() + "/alpha.txt")); // not in the Trash: gone
    QVERIFY(exists(m_left + "/beta.txt"));
    QVERIFY(!app->operations()->canUndo());
}

void TestTui::deleteInTheTrashAsksFirst()
{
    app->selectPath(m_left + "/alpha.txt");
    runJob([&] { keys({Key::of(Key::F8)}); });
    const QString trashed = Trash::homeFilesDir() + "/alpha.txt";
    QVERIFY(exists(trashed));
    keys({Key::character('g'), Key::control('u')});
    type("trash");
    keys({Key::of(Key::Enter)});
    settle();
    QCOMPARE(app->folder(0), Trash::homeFilesDir());
    QVERIFY(screen().contains("Trash"));
    QCOMPARE(app->cursorPath(), trashed);
    // F8 here can only mean "forever": it asks.
    keys({Key::of(Key::F8)});
    QVERIFY(app->isAsking());
    QVERIFY(screen().contains("Permanently delete “alpha.txt”?"));
    keys({Key::character('n')});
    QTest::qWait(100);
    QVERIFY(exists(trashed));
    runJob([&] { keys({Key::of(Key::F8), Key::character('y')}); });
    QVERIFY(!exists(trashed));
}

void TestTui::readOnlyFolderIsLeftAlone()
{
    if (::geteuid() == 0)
        QSKIP("root may write anywhere");
    const QString locked = m_left + "/Documents";
    QVERIFY(QFile::setPermissions(locked, QFile::ReadOwner | QFile::ExeOwner));
    keys({Key::of(Key::Enter)});
    settle();
    QCOMPARE(app->cursorPath(), locked + "/inside.txt");
    keys({Key::of(Key::F8)});
    QCOMPARE(app->message(), QStringLiteral("You can't change items in this folder"));
    keys({Key::character('D')});
    QVERIFY(!app->isAsking()); // not even asked
    keys({Key::of(Key::F6)});
    QCOMPARE(app->message(), QStringLiteral("You can't move items out of this folder"));
    keys({Key::of(Key::F2)});
    QVERIFY(!app->isAsking());
    keys({Key::of(Key::F7)});
    QCOMPARE(app->message(), QStringLiteral("You can't create items in this folder"));
    QVERIFY(!app->isBusy());
    QVERIFY(exists(locked + "/inside.txt"));
    // Copying out of it is fine.
    runJob([&] { keys({Key::of(Key::F5)}); });
    QVERIFY(exists(m_right + "/inside.txt"));
    QFile::setPermissions(locked, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
}

void TestTui::newFolderAndRename()
{
    keys({Key::of(Key::F7)});
    QVERIFY(app->isAsking());
    QVERIFY(screen().contains("New folder:"));
    type("Proj");
    keys({Key::of(Key::Backspace)});
    type("jects");
    keys({Key::of(Key::Enter)});
    QVERIFY(QFileInfo(m_left + "/Projects").isDir());
    settle();
    QTRY_COMPARE(app->cursorPath(), m_left + "/Projects"); // the cursor is on what was made
    // A name that is taken or not allowed: said, nothing made.
    keys({Key::of(Key::F7)});
    type("alpha.txt");
    keys({Key::of(Key::Enter)});
    QCOMPARE(app->message(), QStringLiteral("“alpha.txt” is already there"));
    keys({Key::of(Key::F7)});
    type("a/b");
    keys({Key::of(Key::Enter)});
    QVERIFY(!exists(m_left + "/a"));
    QVERIFY(!app->message().isEmpty());
    // Esc gives up.
    keys({Key::of(Key::F7)});
    type("Never");
    keys({Key::of(Key::Escape)});
    QVERIFY(!exists(m_left + "/Never"));

    // F2: the name is there to edit; never onto another item.
    app->selectPath(m_left + "/alpha.txt");
    keys({Key::of(Key::F2)});
    QVERIFY(screen().contains("Rename to:") && screen().contains("alpha.txt"));
    keys({Key::control('u')});
    type("beta.txt");
    keys({Key::of(Key::Enter)});
    QCOMPARE(app->message(), QStringLiteral("“beta.txt” is already there"));
    QCOMPARE(QFileInfo(m_left + "/beta.txt").size(), 5); // untouched
    QVERIFY(exists(m_left + "/alpha.txt"));
    keys({Key::of(Key::F2), Key::control('u')});
    type("gamma.txt");
    keys({Key::of(Key::Enter)});
    QVERIFY(exists(m_left + "/gamma.txt") && !exists(m_left + "/alpha.txt"));
    settle();
    QTRY_COMPARE(app->cursorPath(), m_left + "/gamma.txt");
    // Both can be undone: the rename, then the new folder (to the Trash).
    runJob([&] { keys({Key::control('z')}); });
    QVERIFY(exists(m_left + "/alpha.txt") && !exists(m_left + "/gamma.txt"));
    runJob([&] { keys({Key::control('z')}); });
    QVERIFY(!exists(m_left + "/Projects"));
}

// Copying onto a name that is taken: asked, and the old one is never lost.
void TestTui::nameClashIsAQuestion()
{
    QFile other(m_right + "/alpha.txt");
    QVERIFY(other.open(QIODevice::WriteOnly));
    other.write("the old one");
    other.close();
    keys({Key::of(Key::Tab), Key::control('r'), Key::of(Key::Tab)});
    settle();
    app->selectPath(m_left + "/alpha.txt");

    QSignalSpy finished(app->operations(), &FileOperations::jobFinished);
    keys({Key::of(Key::F5)});
    QTRY_VERIFY_WITH_TIMEOUT(app->isAsking(), 10000);
    QVERIFY2(screen(140, 24).contains("“alpha.txt” is already in"), qPrintable(screen(140, 24)));
    QVERIFY(screen(140, 24).contains("r: Replace") && screen(140, 24).contains("k: Keep both"));
    keys({Key::character('x'), Key::of(Key::Enter)}); // not answers: still asked
    QVERIFY(app->isAsking());
    keys({Key::character('s')}); // skip
    QTRY_VERIFY_WITH_TIMEOUT(!finished.isEmpty(), 10000);
    settle();
    QCOMPARE(QFileInfo(m_right + "/alpha.txt").size(), 11);

    // Replace: the old one goes to the Trash, it isn't deleted.
    finished.clear();
    keys({Key::of(Key::F5)});
    QTRY_VERIFY_WITH_TIMEOUT(app->isAsking(), 10000);
    keys({Key::character('r')});
    QTRY_VERIFY_WITH_TIMEOUT(!finished.isEmpty(), 10000);
    settle();
    QCOMPARE(QFileInfo(m_right + "/alpha.txt").size(), 5);
    QCOMPARE(QFileInfo(Trash::homeFilesDir() + "/alpha.txt").size(), 11);

    // Esc on the question stops the job; nothing new appears.
    finished.clear();
    keys({Key::of(Key::F5)});
    QTRY_VERIFY_WITH_TIMEOUT(app->isAsking(), 10000);
    keys({Key::of(Key::Escape)});
    QTRY_VERIFY_WITH_TIMEOUT(!finished.isEmpty(), 10000);
    settle();
    QCOMPARE(QDir(m_right).entryList(QDir::Files).size(), 1);
}

void TestTui::hiddenFilesAndGoTo()
{
    keys({Key::character('.')});
    settle();
    QVERIFY(screen().contains(".secret"));
    keys({Key::character('.')});
    settle();
    QVERIFY(!screen().contains(".secret"));
    // Go to: a full path, a name inside the folder, and one that isn't there.
    keys({Key::character('g'), Key::control('u')});
    type("Documents");
    keys({Key::of(Key::Enter)});
    QCOMPARE(app->folder(0), m_left + "/Documents");
    settle();
    keys({Key::control('l'), Key::control('u')});
    type(m_left + "/nowhere");
    keys({Key::of(Key::Enter)});
    QCOMPARE(app->folder(0), m_left + "/Documents");
    QVERIFY(app->message().startsWith("Folder not found"));
    keys({Key::character('~')});
    QCOMPARE(app->folder(0), QDir::cleanPath(QDir::homePath()));
}

void TestTui::startsWithOnePane()
{
    // One folder asked for: one pane, over the whole width.
    app = std::make_unique<App>(m_left, QString());
    settle();
    QVERIFY(!app->isSplit());
    QString text = screen();
    QVERIFY(text.contains("alpha.txt"));
    QVERIFY(!text.contains(QChar(0x2502))); // no line between panes
    QVERIFY(text.contains("F4 Split"));
    QVERIFY(!text.contains("F5 Copy"));

    // Nothing to copy to, nowhere to Tab to.
    app->selectPath(m_left + "/alpha.txt");
    keys({Key::of(Key::F5)});
    settle();
    QVERIFY(!app->isBusy());
    QVERIFY(app->message().contains("F4"));
    QCOMPARE(QDir(m_left).entryList(QDir::Files).size(), 2);
    keys({Key::of(Key::F6)});
    settle();
    QVERIFY(exists(m_left + "/alpha.txt"));
    keys({Key::of(Key::Tab)});
    QCOMPARE(app->activePane(), 0);

    // F4: a second pane, on the same folder; then it is a pane like any other.
    keys({Key::of(Key::F4)});
    settle();
    QVERIFY(app->isSplit());
    QCOMPARE(app->folder(1), m_left);
    text = screen();
    QVERIFY(text.contains(QChar(0x2502)));
    QVERIFY(text.contains("F4 One")); // "One Pane" where there is room
    QVERIFY(screen(140, 24).contains("F4 One Pane"));
    keys({Key::of(Key::Tab)});
    QCOMPARE(app->activePane(), 1);
    app->selectPath(m_left + "/Documents");
    keys({Key::of(Key::Enter)});
    settle();
    QCOMPARE(app->folder(1), m_left + "/Documents");

    // Back to one: the pane you are in stays, and the other keeps its folder.
    keys({Key::of(Key::F4)});
    settle();
    QVERIFY(!app->isSplit());
    QCOMPARE(app->activePane(), 1);
    QVERIFY(screen().contains("inside.txt"));
    QVERIFY(!screen().contains("alpha.txt"));
    // A click where the hidden pane used to be lands in the one shown.
    app->mouse({Tui::Mouse::Press, 3, 4});
    QCOMPARE(app->activePane(), 1);
    keys({Key::of(Key::F4)});
    settle();
    QCOMPARE(app->folder(0), m_left);
    QCOMPARE(app->folder(1), m_left + "/Documents");
}

void TestTui::gridView()
{
    for (int i = 0; i < 9; ++i)
        make(QStringLiteral("file%1.txt").arg(i));
    app = std::make_unique<App>(m_left, QString());
    settle();
    QVERIFY(!app->isGrid(0));
    QVERIFY(screen().contains("F9 Grid"));
    keys({Key::of(Key::F9)});
    QVERIFY(app->isGrid(0));
    const QString text = screen();
    const QStringList lines = text.split('\n');
    // Names side by side, folders first, no column names; the cursor's item
    // has its size at the bottom.
    QVERIFY2(lines.at(2).contains("../") && lines.at(2).contains("Documents/") && lines.at(2).contains("alpha.txt"),
             qPrintable(lines.at(2)));
    QVERIFY(!text.contains("Modified"));
    QVERIFY(text.contains("F9 Icons") && text.contains("F10 Quit")); // what F9 goes to next
    QCOMPARE(app->cursorPath(), m_left + "/Documents");
    QVERIFY2(lines.at(lines.size() - 3).contains("Documents") && lines.at(lines.size() - 3).contains("12 items"),
             qPrintable(lines.at(lines.size() - 3)));

    // Right and left walk along the line; down and up go a line.
    keys({Key::of(Key::Right)});
    QCOMPARE(app->cursorPath(), m_left + "/alpha.txt");
    QVERIFY(screen().contains("5 bytes"));
    QCOMPARE(app->folder(0), m_left); // Right did not open anything
    keys({Key::of(Key::Left)});
    QCOMPARE(app->cursorPath(), m_left + "/Documents");
    QCOMPARE(app->folder(0), m_left); // and Left did not go up
    const QString before = app->cursorPath();
    keys({Key::of(Key::Down)});
    QVERIFY(app->cursorPath() != before);
    keys({Key::of(Key::Up)});
    QCOMPARE(app->cursorPath(), before);
    keys({Key::of(Key::Up), Key::of(Key::Up)}); // the top line: stays in its column
    QCOMPARE(app->cursorPath(), before);
    keys({Key::of(Key::End)});
    QCOMPARE(app->cursorPath(), m_left + "/file8.txt");
    keys({Key::of(Key::Down)});
    QCOMPARE(app->cursorPath(), m_left + "/file8.txt");

    // A click lands on the item under it, a double click opens a folder,
    // a click beside the last column or below the items on nothing.
    Screen s(100, 24);
    app->render(s);
    const int x = int(s.line(2).indexOf("alpha.txt"));
    QVERIFY(x > 0);
    app->mouse({Tui::Mouse::Press, x + 2, 2});
    QCOMPARE(app->cursorPath(), m_left + "/alpha.txt");
    app->mouse({Tui::Mouse::Press, x + 2, 15});
    QCOMPARE(app->cursorPath(), m_left + "/alpha.txt");
    app->mouse({Tui::Mouse::RightPress, x + 2, 2});
    QCOMPARE(app->selectedPaths(), QStringList{m_left + "/alpha.txt"});
    keys({Key::of(Key::Escape)});
    const int dx = int(s.line(2).indexOf("Documents/"));
    app->mouse({Tui::Mouse::Press, dx + 1, 2});
    app->mouse({Tui::Mouse::Press, dx + 1, 2});
    settle();
    QCOMPARE(app->folder(0), m_left + "/Documents");
    QVERIFY(screen().contains("inside.txt"));
    keys({Key::of(Key::Backspace)});
    settle();
    QCOMPARE(app->folder(0), m_left);
    QCOMPARE(app->cursorPath(), m_left + "/Documents"); // where we came out of

    // Commands act on the item under the cursor as in the list.
    app->selectPath(m_left + "/file3.txt");
    runJob([this] { keys({Key::of(Key::F8)}); });
    QVERIFY(!exists(m_left + "/file3.txt"));
    QVERIFY(exists(m_left + "/file2.txt") && exists(m_left + "/file4.txt"));
    runJob([this] { keys({Key::control('z')}); });
    QVERIFY(exists(m_left + "/file3.txt"));

    // More lines than fit: the cursor stays in sight, the wheel scrolls.
    for (int i = 0; i < 300; ++i)
        make(QStringLiteral("more%1.txt").arg(i, 3, 10, QChar('0')));
    keys({Key::control('r')});
    settle();
    QTRY_VERIFY_WITH_TIMEOUT(screen().contains("312 items"), 10000);
    keys({Key::of(Key::End)});
    QVERIFY(screen().contains("more299.txt"));
    QVERIFY(!screen().contains("alpha.txt"));
    keys({Key::of(Key::PageUp), Key::of(Key::PageDown)});
    QCOMPARE(app->cursorPath(), m_left + "/more299.txt");
    keys({Key::of(Key::Home)});
    QVERIFY(screen().contains("../"));
    app->mouse({Tui::Mouse::WheelDown, 5, 5});
    QVERIFY(!screen().contains("../"));
    QVERIFY(!app->cursorPath().isEmpty());

    // The view is remembered, for each pane.
    app = std::make_unique<App>(m_left, m_right);
    settle();
    QVERIFY(app->isGrid(0));
    QVERIFY(!app->isGrid(1));
    const QString both = screen();
    QVERIFY(both.contains("Size")); // the right pane is still a list
    // F9 goes round: grid, big icons, list.
    keys({Key::of(Key::F9)});
    QCOMPARE(app->view(0), App::View::Icons);
    QVERIFY(screen().contains("F9 List"));
    keys({Key::of(Key::F9)});
    QCOMPARE(app->view(0), App::View::List);
    QVERIFY(screen().contains("F9 Grid"));
}

// Big icons: a picture and the name under it for every item, a frame on the
// one under the cursor, and everything else as in the grid.
void TestTui::bigIcons()
{
    make("holiday-photo-from-the-beach.jpeg");
    make("notes");
    make("archive.tar.gz");
    app = std::make_unique<App>(m_left, QString());
    settle();
    keys({Key::of(Key::F9), Key::of(Key::F9)});
    QCOMPARE(app->view(0), App::View::Icons);
    Screen s(100, 30);
    app->render(s);
    const QString text = s.text();
    const QStringList lines = text.split('\n');
    // A line of items takes six screen lines: the frame's, three of picture, two of name.
    QVERIFY2(lines.at(3).contains("▄▄▄") && lines.at(4).contains("███████"), qPrintable(text)); // folders
    QVERIFY2(lines.at(6).contains("..") && lines.at(6).contains("Documents/") && lines.at(6).contains("alpha.txt"), qPrintable(text));
    QVERIFY2(text.contains("│TXT │") || text.contains("│TXT"), qPrintable(text)); // a page says what it is
    QVERIFY(text.contains("JPEG") && text.contains("≡≡≡≡"));                     // "notes" has no ending
    // A long name goes on to a second line, at a natural place; nothing is lost at the edge.
    QVERIFY2(text.contains("holiday-") && text.contains(".jpeg") && !text.contains("holiday-p"), qPrintable(text));
    QVERIFY(!text.contains("holiday-photo-from-the-beach.jpeg"));
    QVERIFY2(text.contains("archive.tar") && text.contains(".gz"), qPrintable(text));
    // The frame is round the item under the cursor (the first item, not "..").
    QCOMPARE(app->cursorPath(), m_left + "/Documents");
    const int frameX = int(lines.at(2).indexOf("┌"));
    QVERIFY2(frameX > 0 && lines.at(8).indexOf("└") == frameX, qPrintable(text));
    QVERIFY(lines.at(6).indexOf("Documents/") > frameX);
    QVERIFY(text.contains("F9 List"));

    // The arrows move four ways; Right and Left neither open nor go up.
    keys({Key::of(Key::Right)});
    QCOMPARE(app->cursorPath(), m_left + "/alpha.txt");
    QCOMPARE(app->folder(0), m_left);
    keys({Key::of(Key::Left), Key::of(Key::Left)});
    QVERIFY(app->cursorPath().isEmpty()); // on ".."
    QCOMPARE(app->folder(0), m_left);
    keys({Key::of(Key::Right)});
    // A click anywhere in a cell (its picture, its name) is on that item; a
    // double click opens it; a right click selects.
    app->render(s);
    const QString nameLine = s.line(6);
    const int alphaX = int(nameLine.indexOf("alpha.txt"));
    QVERIFY(alphaX > 0);
    app->mouse({Tui::Mouse::Press, alphaX + 3, 4}); // on its picture
    QCOMPARE(app->cursorPath(), m_left + "/alpha.txt");
    app->mouse({Tui::Mouse::RightPress, alphaX + 3, 6}); // on its name
    QCOMPARE(app->selectedPaths(), QStringList{m_left + "/alpha.txt"});
    keys({Key::of(Key::Escape)});
    const int docsX = int(nameLine.indexOf("Documents/"));
    app->mouse({Tui::Mouse::Press, docsX + 2, 5});
    app->mouse({Tui::Mouse::Press, docsX + 2, 5});
    settle();
    QCOMPARE(app->folder(0), m_left + "/Documents");
    keys({Key::of(Key::Backspace)});
    settle();
    QCOMPARE(app->cursorPath(), m_left + "/Documents");
    // Below the items: empty space.
    app->mouse({Tui::Mouse::Press, docsX + 2, 26});
    QCOMPARE(app->cursorPath(), m_left + "/Documents");

    // More than fits: Down goes a line of items, the cursor stays in sight, the wheel moves a line.
    for (int i = 0; i < 80; ++i)
        make(QStringLiteral("more%1.txt").arg(i, 2, 10, QChar('0')));
    keys({Key::control('r')});
    settle();
    QTRY_VERIFY_WITH_TIMEOUT(screen(100, 30).contains("86 items"), 10000);
    keys({Key::of(Key::End)});
    QCOMPARE(app->cursorPath(), m_left + "/notes");
    QVERIFY(screen(100, 30).contains("notes"));
    QVERIFY(!screen(100, 30).contains("Documents/"));
    keys({Key::of(Key::Home)});
    QVERIFY(screen(100, 30).contains("Documents/"));
    app->mouse({Tui::Mouse::WheelDown, 5, 5});
    QVERIFY(!screen(100, 30).contains("Documents/"));

    // Remembered; a narrow pane still shows something sensible.
    app = std::make_unique<App>(m_left, m_right);
    settle();
    QCOMPARE(app->view(0), App::View::Icons);
    QCOMPARE(app->view(1), App::View::List);
    QVERIFY(screen(40, 12).contains("███████"));
}

void TestTui::shellInTheFolder()
{
    // Where there is no shell panel (no libvterm), F12 or Ctrl+` asks for a
    // shell in the pane's folder to take the terminal; the interface itself
    // starts nothing.
    app->setEmbeddedShell(false);
    QSignalSpy shell(app.get(), &App::shellRequested);
    keys({Key::control('`')});
    QCOMPARE(shell.size(), 1);
    QCOMPARE(shell.first().first().toString(), m_left);
    // Not while a name is being typed.
    keys({Key::of(Key::F7), Key::control('`'), Key::of(Key::Escape)});
    QCOMPARE(shell.size(), 1);

    // Back from the shell: what it made is there, and the pane is where the
    // shell ended up. The other pane stays.
    make("made-in-shell.txt");
    app->shellClosed(m_left);
    QTRY_VERIFY_WITH_TIMEOUT(screen().contains("made-in-shell.txt"), 10000);
    app->shellClosed(m_left + "/Documents");
    settle();
    QCOMPARE(app->folder(0), m_left + "/Documents");
    QCOMPARE(app->folder(1), m_right);
    keys({Key::of(Key::Left)}); // Alt+Left would go back; plain Left goes up
    settle();
    QCOMPARE(app->folder(0), m_left);
    // A folder that is gone, or none: it stays.
    app->shellClosed(m_left + "/no-such-folder");
    app->shellClosed(QString());
    settle();
    QCOMPARE(app->folder(0), m_left);
    // In a place, the shell starts at home.
    QDir().mkpath(Trash::homeFilesDir());
    keys({Key::character('g'), Key::control('u')});
    type("trash");
    keys({Key::of(Key::Enter)});
    settle();
    QCOMPARE(app->folder(0), Trash::homeFilesDir());
    keys({Key::control('`')});
    QCOMPARE(shell.size(), 2);
    QCOMPARE(shell.last().first().toString(), QDir::homePath());
    const QString trash = app->folder(0);
    app->shellClosed(m_left); // and the Trash stays open
    settle();
    QCOMPARE(app->folder(0), trash);
}

void TestTui::shellPanel()
{
    if (!Tui::Shell::available())
        QSKIP("built without libvterm: no shell panel");
    qputenv("SHELL", "/bin/sh");
    qputenv("PS1", "ready> ");
    QSignalSpy outside(app.get(), &App::shellRequested);
    const QString before = screen();
    QVERIFY(before.contains("F12 Term")); // "Terminal" where there is room
    QVERIFY(!before.contains("ready>"));

    // F12: a shell under the panes, in the pane's folder, and typing goes to it.
    keys({Key::of(Key::F12)});
    QVERIFY(app->isShellOpen());
    QVERIFY(app->isInShell());
    QCOMPARE(outside.size(), 0); // the terminal isn't handed over
    QTRY_VERIFY_WITH_TIMEOUT(screen().contains("ready>"), 10000);
    QStringList lines = screen().split('\n');
    int title = -1;
    for (int y = 0; y < lines.size(); ++y) {
        if (lines.at(y).contains("Terminal") && lines.at(y).contains("left"))
            title = y;
    }
    QVERIFY2(title >= 10 && title <= 12, qPrintable(screen())); // the lower half of 24 lines
    QVERIFY(lines.at(1).contains("left") && lines.at(4).contains("Documents/")); // the files are still above
    QVERIFY(lines.last().contains("F12 Files") && !lines.last().contains("F8 Trash"));
    Screen s(100, 24);
    app->render(s);
    QVERIFY(s.cursorY() > title); // the text cursor is the shell's

    // What is typed is the shell's: "q" doesn't quit, F8 and Delete trash nothing.
    QSignalSpy quit(app.get(), &App::quitRequested);
    keys({Key::of(Key::F8), Key::of(Key::Delete), Key::character('q'), Key::control('u')}); // typed, then wiped
    type("echo sum-$((40+2)) > made.txt; cat made.txt");
    keys({Key::of(Key::Enter)});
    QTRY_VERIFY_WITH_TIMEOUT(screen().contains("sum-42"), 10000);
    QCOMPARE(quit.size(), 0);
    QVERIFY(exists(m_left + "/alpha.txt") && exists(m_left + "/Documents"));
    QTRY_VERIFY_WITH_TIMEOUT(exists(m_left + "/made.txt"), 10000); // it ran in the pane's folder

    // Colours are the shell's own.
    type("printf '\\033[31mred-text\\033[0m\\n'");
    keys({Key::of(Key::Enter)});
    QTRY_VERIFY_WITH_TIMEOUT(screen().contains("red-text"), 10000);
    app->render(s);
    bool red = false;
    for (int y = 0; y < s.height(); ++y) {
        const int x = int(s.line(y).indexOf("red-text"));
        if (x >= 0 && s.at(x, y).style.raw && s.at(x, y).style.fg == 1)
            red = true;
    }
    QVERIFY(red);

    // Ctrl+Up: the files come to where the shell is.
    type("cd Documents");
    keys({Key::of(Key::Enter)});
    Key ctrlUp = Key::of(Key::Up);
    ctrlUp.ctrl = true;
    QTRY_VERIFY_WITH_TIMEOUT((keys({ctrlUp}), app->folder(0) == m_left + "/Documents"), 10000);
    settle();
    QVERIFY(app->isInShell());

    // F12 again: back to the files, the panel stays. Keys are the files' again.
    keys({Key::of(Key::F12)});
    QVERIFY(app->isShellOpen());
    QVERIFY(!app->isInShell());
    QVERIFY(screen().contains("F8 Trash") && screen().contains("ready>"));
    keys({Key::of(Key::Backspace)});
    settle();
    QCOMPARE(app->folder(0), m_left);
    QVERIFY(screen().contains("made.txt"));
    // A click on the panel goes into it, a click on a pane comes back.
    app->mouse({Tui::Mouse::Press, 10, title + 2});
    QVERIFY(app->isInShell());
    app->mouse({Tui::Mouse::Press, 10, 4});
    QVERIFY(!app->isInShell());
    QCOMPARE(app->folder(0), m_left);

    keys({Key::of(Key::F12)});
    QVERIFY(app->isInShell());

    // exit: the panel is gone and the files have the whole height again.
    type("exit");
    keys({Key::of(Key::Enter)});
    QTRY_VERIFY_WITH_TIMEOUT(!app->isShellOpen(), 10000);
    QVERIFY(!app->isInShell());
    QVERIFY(!screen().contains("ready>"));
    QVERIFY(screen(140, 24).contains("F12 Terminal"));

    // Quitting with the shell open ends it.
    keys({Key::of(Key::F12)});
    QTRY_VERIFY_WITH_TIMEOUT(screen().contains("ready>"), 10000);
    app.reset();
    app = std::make_unique<App>(m_left, m_right);
    settle();
    QVERIFY(!app->isShellOpen());
}

void TestTui::quitWaitsForTheWork()
{
    QSignalSpy quit(app.get(), &App::quitRequested);
    keys({Key::of(Key::F10)});
    QCOMPARE(quit.size(), 1);
    keys({Key::character('q')});
    QCOMPARE(quit.size(), 2);
    // While typing a name, "q" is a letter.
    keys({Key::of(Key::F7), Key::character('q'), Key::of(Key::Escape)});
    QCOMPARE(quit.size(), 2);
    // With a question open (a copy waiting for an answer), it doesn't leave.
    QFile other(m_right + "/alpha.txt");
    QVERIFY(other.open(QIODevice::WriteOnly));
    other.close();
    app->selectPath(m_left + "/alpha.txt");
    QSignalSpy finished(app->operations(), &FileOperations::jobFinished);
    keys({Key::of(Key::F5)});
    QTRY_VERIFY_WITH_TIMEOUT(app->isAsking(), 10000);
    keys({Key::of(Key::F10)}); // not an answer
    QCOMPARE(quit.size(), 2);
    QVERIFY(app->isAsking());
    keys({Key::character('s')});
    QTRY_VERIFY_WITH_TIMEOUT(!finished.isEmpty(), 10000);
    settle();
}

// The mouse: a click goes to a row (and to its pane), a double click opens a
// folder, the right button selects, the wheel scrolls, the key line's words
// are buttons; and a question can't be answered by a stray click.
void TestTui::mouseClicksAndDoubleClicks()
{
    using Tui::Mouse;
    screen(); // a frame: the mouse acts on what is drawn
    // The left pane: heading, column names, ".." on row 3, then the items from 4.
    app->mouse({Mouse::Press, 10, 6});
    QCOMPARE(app->cursorPath(), m_left + "/beta.txt");
    QCOMPARE(app->folder(0), m_left);
    // A click in the right pane makes it the active one.
    app->mouse({Mouse::Press, 70, 10});
    QCOMPARE(app->activePane(), 1);
    app->mouse({Mouse::Press, 10, 4});
    QCOMPARE(app->activePane(), 0);
    QCOMPARE(app->cursorPath(), m_left + "/Documents");
    QCOMPARE(app->folder(0), m_left); // one click doesn't open
    // Two slow clicks are two clicks.
    QTest::qWait(500);
    app->mouse({Mouse::Press, 10, 4});
    QCOMPARE(app->folder(0), m_left);
    // A double click on a folder opens it.
    app->mouse({Mouse::Press, 10, 4});
    QCOMPARE(app->folder(0), m_left + "/Documents");
    settle();
    screen();
    QCOMPARE(app->cursorPath(), m_left + "/Documents/inside.txt");
    // The way back up, with the mouse: a double click on "..".
    QTest::qWait(500);
    app->mouse({Mouse::Press, 10, 3});
    QCOMPARE(app->folder(0), m_left + "/Documents"); // one click only goes to the row
    QVERIFY(app->cursorPath().isEmpty());
    app->mouse({Mouse::Press, 10, 3});
    QCOMPARE(app->folder(0), m_left);
    settle();
    screen();
    QCOMPARE(app->cursorPath(), m_left + "/Documents");
    // Or one click on the folder's name in the heading.
    keys({Key::of(Key::Enter)});
    settle();
    screen();
    app->mouse({Mouse::Press, 6, 1});
    QCOMPARE(app->folder(0), m_left);
    settle();
    screen();
    // A click on one row, then quickly on another, is not a double click.
    QTest::qWait(500);
    app->mouse({Mouse::Press, 10, 5});
    app->mouse({Mouse::Press, 10, 4});
    QCOMPARE(app->folder(0), m_left);
    QTest::qWait(500);

    // The right button selects, without opening; ".." can't be selected.
    app->mouse({Mouse::RightPress, 10, 5});
    app->mouse({Mouse::RightPress, 10, 6});
    app->mouse({Mouse::RightPress, 10, 3});
    QCOMPARE(app->selectedPaths(), (QStringList{m_left + "/alpha.txt", m_left + "/beta.txt"}));
    app->mouse({Mouse::RightPress, 10, 5});
    QCOMPARE(app->selectedPaths(), QStringList{m_left + "/beta.txt"});
    keys({Key::of(Key::Escape)});
    // ".." is no target: with the cursor on it the commands do nothing.
    app->mouse({Mouse::Press, 10, 3});
    keys({Key::of(Key::F8), Key::character('D'), Key::of(Key::F5), Key::of(Key::F2)});
    QVERIFY(!app->isAsking() && !app->isBusy());
    QVERIFY(exists(m_left + "/Documents") && exists(m_left + "/alpha.txt"));
    QTest::qWait(500);
    // Below the last row: nothing to go to.
    app->mouse({Mouse::Press, 10, 12});
    QCOMPARE(app->folder(0), m_left);

    // The wheel scrolls a long folder.
    for (int i = 0; i < 60; ++i)
        make(QStringLiteral("many/file%1.txt").arg(i, 2, 10, QLatin1Char('0')));
    keys({Key::control('l'), Key::control('u')});
    type(m_left + "/many");
    keys({Key::of(Key::Enter)});
    settle();
    QVERIFY(screen().contains("file00.txt"));
    app->mouse({Mouse::WheelDown, 10, 8});
    QVERIFY(!screen().contains("file00.txt"));
    QVERIFY(screen().contains("file02.txt"));
    app->mouse({Mouse::WheelUp, 10, 8});
    QVERIFY(screen().contains("file00.txt"));

    // The key line: a click on "F7 New Folder" is F7.
    const QString text = screen();
    const QString last = text.split('\n').last();
    const int at = int(last.indexOf("F7 ")) + 3; // "New Folder", or "Folder" where the line is narrow
    QVERIFY(at > 0);
    app->mouse({Mouse::Press, at + 2, 23});
    QVERIFY(app->isAsking());
    QVERIFY(screen().contains("New folder:"));
    keys({Key::of(Key::Escape)});

    // While a question is open the mouse does nothing: not "yes", not "no".
    app->selectPath(m_left + "/many/file05.txt");
    keys({Key::character('D')});
    QVERIFY(app->isAsking());
    for (int y = 0; y < 24; ++y)
        app->mouse({Mouse::Press, 20, y});
    QVERIFY(app->isAsking());
    QVERIFY(exists(m_left + "/many/file05.txt"));
    keys({Key::character('n')});
    QVERIFY(exists(m_left + "/many/file05.txt"));
}

// Enter or a double click on a file: its app on a desktop, the terminal's
// own viewer for text where there is none; a disguised program, never.
void TestTui::opensFiles()
{
    using Tui::Mouse;
    const QString text = m_left + "/alpha.txt";
    const QString binary = make("data.bin", QByteArrayLiteral("\x00\x01\x02\x03 not text \x00\xff"));
    const QString disguised = make("invoice.txt", QByteArray("\x7f" "ELF\x02\x01\x01\0\0\0\0\0\0\0\0\0\x02\0\x3e\0", 20)
                                                      + QByteArray(200, '\0'));
    keys({Key::control('r')});
    settle();
    QTRY_VERIFY_WITH_TIMEOUT(screen().contains("invoice.txt") && screen().contains("data.bin"), 10000);
    QSignalSpy view(app.get(), &App::viewRequested);
    const QString log = QFileInfo(qEnvironmentVariable("ARGONAUT_GIO_PROGRAM")).path() + "/gio.log";
    auto launched = [&] {
        QFile f(log);
        return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()) : QString();
    };

    // No desktop (a server over SSH): text is shown in the terminal.
    qunsetenv("DISPLAY");
    qunsetenv("WAYLAND_DISPLAY");
    app->selectPath(text);
    QCOMPARE(app->cursorPath(), text);
    keys({Key::of(Key::Enter)});
    QCOMPARE(view.size(), 1);
    QCOMPARE(view.first().at(0).toString(), text);
    QCOMPARE(app->folder(0), m_left); // still here
    app->selectPath(binary);
    QCOMPARE(app->cursorPath(), binary);
    keys({Key::of(Key::Enter)});
    QCOMPARE(view.size(), 1);
    QVERIFY2(app->message().contains("no desktop"), qPrintable(app->message()));
    QVERIFY(launched().isEmpty()); // no app was started

    // A desktop: the app that is the default for the file's real type.
    qputenv("DISPLAY", ":99");
    app->selectPath(text);
    QCOMPARE(app->cursorPath(), text);
    keys({Key::of(Key::Enter)});
    QCOMPARE(app->message(), QStringLiteral("Opening “alpha.txt” with Test Viewer"));
    QTRY_VERIFY_WITH_TIMEOUT(launched().contains("launch"), 5000);
    QVERIFY(launched().contains(text));
    QCOMPARE(view.size(), 1); // not also in the terminal

    // A double click does the same as Enter.
    QFile::remove(log);
    screen();
    QTest::qWait(500);
    int row = -1;
    {
        Screen s(100, 24);
        app->render(s);
        for (int y = 3; y < 20 && row < 0; ++y) { // the list's rows, not the message line
            if (s.line(y).left(48).contains("alpha.txt"))
                row = y;
        }
    }
    QVERIFY(row > 0);
    app->mouse({Mouse::Press, 10, row});
    QVERIFY(launched().isEmpty()); // one click only goes there
    app->mouse({Mouse::Press, 10, row});
    QTRY_VERIFY_WITH_TIMEOUT(launched().contains(text), 5000);

    // A program named like a document is never opened, desktop or not.
    QFile::remove(log);
    app->selectPath(disguised);
    QCOMPARE(app->cursorPath(), disguised);
    keys({Key::of(Key::Enter)});
    QVERIFY2(app->message().contains("program disguised as a document"), qPrintable(app->message()));
    QTest::qWait(300);
    QVERIFY(launched().isEmpty());
    QCOMPARE(view.size(), 1);
    qunsetenv("DISPLAY");
    keys({Key::of(Key::Enter)});
    QVERIFY(app->message().contains("program disguised as a document"));
    QCOMPARE(view.size(), 1);

    // F3 reads a text file in the terminal, wherever we are; nothing else.
    app->selectPath(text);
    QCOMPARE(app->cursorPath(), text);
    keys({Key::of(Key::F3)});
    QCOMPARE(view.size(), 2);
    app->selectPath(binary);
    QCOMPARE(app->cursorPath(), binary);
    keys({Key::of(Key::F3)});
    QCOMPARE(app->message(), QStringLiteral("“data.bin” isn't a text file"));
    app->selectPath(m_left + "/Documents");
    keys({Key::of(Key::F3)});
    QCOMPARE(view.size(), 2);
    QCOMPARE(app->folder(0), m_left);
}

int main(int argc, char **argv)
{
    TestSupport::Sandbox sandbox;
    if (!sandbox.isValid())
        return 1;
    // No real app is ever started: a stand-in for `gio` that names one app
    // for every type and writes down what it is asked to launch.
    {
        const QString root = sandbox.path();
        QDir().mkpath(root + "/data/applications");
        QFile desktop(root + "/data/applications/viewer.desktop");
        if (!desktop.open(QIODevice::WriteOnly))
            return 1;
        desktop.write("[Desktop Entry]\nType=Application\nName=Test Viewer\nExec=true %f\n");
        desktop.close();
        QFile gio(root + "/gio");
        if (!gio.open(QIODevice::WriteOnly))
            return 1;
        gio.write("#!/bin/sh\n"
                  "if [ \"$1\" = mime ]; then\n"
                  "  printf 'Default application for \"%s\": viewer.desktop\\nRegistered applications:\\n\\tviewer.desktop\\n"
                  "Recommended applications:\\n\\tviewer.desktop\\n' \"$2\"\n"
                  "else\n"
                  "  echo \"$@\" >> \"$(dirname \"$0\")/gio.log\"\n"
                  "fi\n");
        gio.close();
        QFile::setPermissions(root + "/gio", QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        qputenv("ARGONAUT_GIO_PROGRAM", QFile::encodeName(root + "/gio"));
        qputenv("ARGONAUT_REMEMBER_RECENT", "1");
    }
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName("argonaut-test");
    QCoreApplication::setApplicationName("tui");
    TestTui test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_tui.moc"
