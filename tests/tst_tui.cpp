// Tests for the terminal interface (Tui::App): what is on screen, what the
// keys do, and above all that nothing permanent happens without a "y".
// The interface draws into a grid of cells, so no terminal is needed.

#include "App.h"
#include "Shell.h"
#include "Bookmarks.h"
#include "DirectoryModel.h"
#include "Extensions.h"
#include "FileOperations.h"
#include "Screen.h"
#include "TestSupport.h"
#include "Trash.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QSettings>
#include <QImage>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QSignalSpy>
#include <QtTest>

#include <memory>
#include <unistd.h>

using Tui::App;
using Tui::Key;
using Tui::Screen;

// Delete forever: Shift+Delete (a plain "D" is a letter now: typing goes to a name).
static Tui::Key shiftDelete()
{
    Tui::Key key = Tui::Key::of(Tui::Key::Delete);
    key.shift = true;
    return key;
}

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
    void scrollbars();
    void placesAtTheLeft();
    void previewAtTheRight();
    void unmountedDrive();
    void commandPalette();
    void menus();
    void extensionResultInThePreview();
    void themes();
    void effects();
    void typingGoesToAName();
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
    QSettings().setValue("tui/sidebar", false); // and without the places at the left, unless it is about them
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
    QVERIFY(lines.last().contains("F5 Copy") && lines.last().contains("^Q Quit"));
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
    for (const Key &startKey : {shiftDelete(), [] { Key k = Key::of(Key::Delete); k.shift = true; return k; }()}) {
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
        keys({shiftDelete(), no});
        QTest::qWait(50);
        QVERIFY(exists(m_left + "/alpha.txt"));
        QVERIFY(!app->isAsking());
    }
    // Only "y" deletes, and only what was asked about.
    runJob([&] { keys({shiftDelete(), Key::character('y')}); });
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
    keys({Key::control('l'), Key::control('u')});
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
    keys({shiftDelete()});
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
    keys({Key::control('l'), Key::control('u')});
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
    QVERIFY(text.contains("F9 Icons") && text.contains("^Q Quit")); // what F9 goes to next
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
    const int frameX = int(lines.at(2).indexOf("╭"));
    QVERIFY2(frameX > 0 && lines.at(8).indexOf("╰") == frameX, qPrintable(text));
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

// A scrollbar down a pane's right edge when there is more than fits, in
// every view, and in a help longer than the screen. None when all fits.
void TestTui::scrollbars()
{
    app = std::make_unique<App>(m_left, QString());
    settle();
    // What a pane's last column shows, top to bottom: "│" track, "█" handle.
    auto bar = [this](int width = 100, int height = 24) {
        Screen s(width, height);
        app->render(s);
        QString column;
        for (int y = 1; y < height - 3; ++y)
            column += QString::fromUcs4(&s.at(width - 1, y).ch, 1);
        return column.trimmed();
    };
    QVERIFY2(bar().isEmpty(), qPrintable(bar())); // four items: nothing to scroll

    for (int i = 0; i < 200; ++i)
        make(QStringLiteral("file%1.txt").arg(i, 3, 10, QChar('0')));
    keys({Key::control('r')});
    settle();
    QTRY_VERIFY_WITH_TIMEOUT(screen().contains("203 items"), 10000);

    for (const char *view : {"list", "grid", "icons"}) {
        keys({Key::of(Key::Home)});
        const QString top = bar();
        QVERIFY2(top.startsWith(QChar(0x2588)) && top.endsWith(QChar(0x2502)), view); // the handle is at the top
        QVERIFY2(top.count(QChar(0x2588)) < top.size(), view);
        keys({Key::of(Key::End)});
        const QString bottom = bar();
        QVERIFY2(bottom.startsWith(QChar(0x2502)) && bottom.endsWith(QChar(0x2588)), view); // at the bottom
        QCOMPARE(bottom.size(), top.size());
        QCOMPARE(bottom.count(QChar(0x2588)), top.count(QChar(0x2588)));

        // A click on the bar goes to that part: its top is the start, and
        // the cursor comes along; the pane's files are still what is clicked elsewhere.
        Screen s(100, 24);
        app->render(s);
        int firstY = -1, lastY = -1;
        for (int y = 1; y < 21; ++y) {
            if (s.at(99, y).ch == 0x2502 || s.at(99, y).ch == 0x2588) {
                if (firstY < 0)
                    firstY = y;
                lastY = y;
            }
        }
        QVERIFY2(firstY > 0 && lastY > firstY, view);
        QVERIFY2(!screen().contains("Documents/"), view);
        app->mouse({Tui::Mouse::Press, 99, firstY});
        QVERIFY2(screen().contains("Documents/"), view);
        QVERIFY2(bar().startsWith(QChar(0x2588)), view);
        app->mouse({Tui::Mouse::Press, 99, lastY});
        QVERIFY2(screen().contains("file199.txt"), view);
        QVERIFY2(!app->cursorPath().isEmpty() && screen().contains(QFileInfo(app->cursorPath()).fileName().left(8)), view);
        app->mouse({Tui::Mouse::Press, 99, (firstY + lastY) / 2});
        QVERIFY2(!screen().contains("Documents/") && !screen().contains("file199.txt"), view);
        keys({Key::of(Key::F9)}); // the next view
    }
    QCOMPARE(app->view(0), App::View::List);

    // Two panes: each has its own, at its own edge.
    keys({Key::of(Key::F4)});
    settle();
    Screen two(100, 24);
    app->render(two);
    QString middle, right;
    for (int y = 3; y < 20; ++y) {
        middle += QString::fromUcs4(&two.at(48, y).ch, 1);
        right += QString::fromUcs4(&two.at(99, y).ch, 1);
    }
    QVERIFY2(middle.contains(QChar(0x2588)) && right.contains(QChar(0x2588)), qPrintable(middle + "|" + right));
    keys({Key::of(Key::F4)});

    // The help: on a short screen it scrolls and shows a bar; a key that
    // isn't for moving closes it. On a tall screen any key closes it.
    keys({Key::of(Key::F1)});
    const QString first = screen(100, 14);
    QVERIFY(first.contains("Argonaut in the terminal"));
    QVERIFY(!first.contains("Press any key."));
    QVERIFY(first.contains("the wheel"));
    QVERIFY(!bar(100, 14).isEmpty());
    keys({Key::of(Key::PageDown)});
    QVERIFY(app->isAsking());
    QVERIFY(!screen(100, 14).contains("Argonaut in the terminal"));
    keys({Key::of(Key::End)});
    QVERIFY(screen(100, 14).contains("Press any key."));
    app->mouse({Tui::Mouse::WheelUp, 5, 5});
    QVERIFY(!screen(100, 14).contains("Press any key."));
    keys({Key::character('x')});
    QVERIFY(!app->isAsking());
    keys({Key::of(Key::F1)});
    QVERIFY(screen(100, 60).contains("Press any key."));
    keys({Key::of(Key::Down)}); // it all fits: Down is "any key"
    QVERIFY(!app->isAsking());
}

// The sidebar: the same places as the window's (Recent, Starred, the
// standard folders, the Trash, bookmarks, drives), at the left of the files.
void TestTui::placesAtTheLeft()
{
    Bookmarks::add(m_left + "/Documents");
    app = std::make_unique<App>(m_left, QString());
    settle();
    QVERIFY(!app->isSidebarShown()); // the tests' setting; on unless switched off
    QVERIFY(!screen().contains("Places"));
    app->setSidebarShown(true);
    Screen s(100, 30);
    app->render(s);
    QStringList lines = s.text().split('\n');
    QVERIFY2(lines.at(1).startsWith(" Places") && lines.at(1).contains("left"), qPrintable(s.text())); // beside the pane's heading
    auto rowOf = [&](const QString &name) {
        for (int y = 2; y < lines.size(); ++y) {
            if (lines.at(y).left(21).trimmed() == name)
                return y;
        }
        return -1;
    };
    QVERIFY(rowOf("Recent") == 2 && rowOf("Starred") == 3);
    QVERIFY(rowOf("Home") > 0 && rowOf("Trash") > rowOf("Home"));
    QVERIFY2(rowOf("Documents") > rowOf("Trash") + 1, qPrintable(s.text())); // the bookmark, after a line
    QVERIFY(lines.at(rowOf("Trash") + 1).contains(QString(8, QChar(0x2500))));
    QVERIFY(rowOf("Computer") > rowOf("Documents"));
    // The files are beside it, whole: nothing of a pane is under the sidebar.
    QVERIFY(lines.at(4).indexOf("Documents/") > 22);
    QVERIFY(lines.at(4).contains("1 item"));
    QVERIFY(!app->isInSidebar());

    // A click on a place opens it in the pane; its row is then the marked one.
    app->mouse({Tui::Mouse::Press, 4, rowOf("Home")});
    settle();
    QCOMPARE(app->folder(0), QDir::homePath());
    QVERIFY(!app->isInSidebar());
    app->render(s);
    QCOMPARE(s.at(2, rowOf("Home")).style.paper, Tui::Paper::CursorIdle);
    QCOMPARE(s.at(2, rowOf("Trash")).style.paper, Tui::Paper::Side); // an ordinary row: the places' own background
    // A click on a file still lands on the file.
    app->mouse({Tui::Mouse::Press, 4, rowOf("Documents")}); // the bookmark
    settle();
    QCOMPARE(app->folder(0), m_left + "/Documents");
    app->render(s);
    const int fileX = int(s.line(4).indexOf("inside.txt"));
    QVERIFY(fileX > 22);
    app->mouse({Tui::Mouse::Press, fileX + 1, 4});
    QCOMPARE(app->cursorPath(), m_left + "/Documents/inside.txt");

    // Tab goes to the places; Up and Down move over the lines between the
    // groups; Enter opens one in the pane and the keys are the files' again.
    keys({Key::of(Key::Tab)});
    QVERIFY(app->isInSidebar());
    app->render(s);
    QCOMPARE(s.at(2, rowOf("Documents")).style.paper, Tui::Paper::Cursor); // on the open folder's row
    keys({Key::of(Key::Up)});
    app->render(s);
    QCOMPARE(s.at(2, rowOf("Trash")).style.paper, Tui::Paper::Cursor); // over the line
    // The files' keys do nothing to the files from here.
    keys({Key::of(Key::F8), shiftDelete(), Key::of(Key::F7)});
    settle();
    QVERIFY(!app->isAsking());
    QVERIFY(exists(m_left + "/Documents/inside.txt"));
    keys({Key::of(Key::Enter)});
    settle();
    QCOMPARE(app->folder(0), Trash::homeFilesDir());
    QVERIFY(!app->isInSidebar());
    keys({Key::of(Key::Tab), Key::of(Key::Home), Key::of(Key::Enter)});
    settle();
    QCOMPARE(app->folder(0), DirectoryModel::recentPath());
    // Esc leaves the places without going anywhere; with two panes Tab goes round all three.
    keys({Key::of(Key::Tab), Key::of(Key::Escape)});
    QVERIFY(!app->isInSidebar());
    keys({Key::of(Key::F4)});
    settle();
    QCOMPARE(app->activePane(), 0);
    keys({Key::of(Key::Tab)});
    QCOMPARE(app->activePane(), 1);
    keys({Key::of(Key::Tab)});
    QVERIFY(app->isInSidebar());
    keys({Key::of(Key::Tab)});
    QVERIFY(!app->isInSidebar());
    QCOMPARE(app->activePane(), 0);
    Key back = Key::of(Key::Tab);
    back.shift = true;
    keys({back});
    QVERIFY(app->isInSidebar());
    // A place opens in the pane you came from.
    keys({back});
    QCOMPARE(app->activePane(), 1);
    app->render(s);
    lines = s.text().split('\n');
    app->mouse({Tui::Mouse::Press, 4, rowOf("Home")});
    settle();
    QCOMPARE(app->folder(1), QDir::homePath());
    keys({Key::of(Key::F4)});

    // Ctrl+B hides it and shows it; that is remembered. A narrow terminal has no room for it.
    auto headed = [this](int width) { return screen(width, 30).split('\n').at(1).startsWith(" Places"); };
    QVERIFY(headed(100));
    QVERIFY(!headed(63));
    keys({Key::control('b')});
    QVERIFY(!app->isSidebarShown());
    QVERIFY(!headed(100));
    app = std::make_unique<App>(m_left, QString());
    settle();
    QVERIFY(!app->isSidebarShown());
    keys({Key::control('b')});
    QVERIFY(screen(100, 30).split('\n').at(1).startsWith(" Places"));
    Bookmarks::remove(m_left + "/Documents");
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
    keys({Key::control('l'), Key::control('u')});
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
    QVERIFY2(title >= 14 && title <= 15, qPrintable(screen())); // about the lower third (35%) of 24 lines
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

    // With the places at the left, the terminal is under the files only:
    // the places keep their whole height, as the window's sidebar does.
    app->setSidebarShown(true);
    keys({Key::of(Key::F12)});
    QTRY_VERIFY_WITH_TIMEOUT(screen(100, 30).contains("ready>"), 10000);
    {
        Screen wide(100, 30);
        app->render(wide);
        const QStringList rows = wide.text().split('\n');
        int bar = -1, prompt = -1, trash = -1;
        for (int y = 0; y < rows.size(); ++y) {
            if (rows.at(y).mid(22).contains("Terminal") && rows.at(y).mid(22).contains("left"))
                bar = y;
            if (rows.at(y).contains("ready>"))
                prompt = y;
            if (rows.at(y).left(21).trimmed() == "Trash")
                trash = y;
        }
        QVERIFY2(bar > 5 && prompt > bar && trash > 0, qPrintable(wide.text()));
        QVERIFY(rows.at(bar).left(21).trimmed() != "Terminal");      // not over the places
        QCOMPARE(rows.at(prompt).indexOf("ready>"), 22);              // the shell starts where the files do
        QCOMPARE(wide.at(21, 27).ch, char32_t(0x2502));               // the places' edge runs to the bottom
        QVERIFY(wide.cursorX() >= 22);
        // A click on a place beside the terminal is still a place; one on the terminal goes into it.
        keys({Key::of(Key::F12)});
        QVERIFY(!app->isInShell());
        app->mouse({Tui::Mouse::Press, 4, bar + 1 < trash ? trash : bar + 1});
        QVERIFY(!app->isInShell());
        app->mouse({Tui::Mouse::Press, 40, prompt});
        QVERIFY(app->isInShell());
        type("exit");
        keys({Key::of(Key::Enter)});
        QTRY_VERIFY_WITH_TIMEOUT(!app->isShellOpen(), 10000);
    }
    app->setSidebarShown(false);

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
    keys({Key::control('q')});
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

    // The right button, as in the window: the item becomes the selection
    // and its menu opens at the pointer, without opening the item.
    app->mouse({Mouse::RightPress, 10, 5});
    QVERIFY(app->isContextMenuOpen());
    QCOMPARE(app->selectedPaths(), QStringList{m_left + "/alpha.txt"});
    QCOMPARE(app->menuNames().mid(0, 2), (QStringList{"Open", "Rename…"}));
    QVERIFY(app->menuNames().contains("Move to Trash") && app->menuNames().contains("Delete Permanently…"));
    QVERIFY(!app->menuNames().contains("New Folder…"));
    QString menuText = screen(100, 30);
    QVERIFY2(menuText.section('\n', 6, 6).contains("╭──") && menuText.section('\n', 7, 7).contains("Open"), qPrintable(menuText));
    // A right click elsewhere while it is open only closes it; so does Esc.
    app->mouse({Mouse::RightPress, 10, 6});
    QVERIFY(!app->isContextMenuOpen());
    QCOMPARE(app->selectedPaths(), QStringList{m_left + "/alpha.txt"});
    // On another item: that one is the selection now.
    app->mouse({Mouse::RightPress, 10, 6});
    QCOMPARE(app->selectedPaths(), QStringList{m_left + "/beta.txt"});
    keys({Key::of(Key::Escape)});
    QVERIFY(!app->isAsking());
    // On one of several selected items: they all stay, and the menu is about them all.
    keys({Key::control('a')});
    const int all = int(app->selectedPaths().size());
    QVERIFY(all >= 3);
    app->mouse({Mouse::RightPress, 10, 5});
    QCOMPARE(int(app->selectedPaths().size()), all);
    keys({Key::of(Key::Escape)});
    // A command of the menu is the same as its key: deleting for good asks, and only "y" goes on.
    app->mouse({Mouse::RightPress, 10, 5}); // alpha is selected with the rest: they stay
    keys({Key::of(Key::Escape), Key::of(Key::Escape)}); // the menu, then the selection
    QVERIFY(app->selectedPaths().isEmpty());
    app->mouse({Mouse::RightPress, 10, 5});
    keys({Key::character('d'), Key::of(Key::Enter)});
    QVERIFY(!app->isContextMenuOpen());
    QVERIFY(app->isAsking());
    keys({Key::of(Key::Enter)}); // not "y"
    settle();
    QVERIFY(exists(m_left + "/alpha.txt"));
    // Trash from the menu, with the mouse on its line; undo brings it back.
    app->mouse({Mouse::RightPress, 10, 5});
    menuText = screen(100, 30);
    int trashY = -1;
    for (int y = 0; y < 30 && trashY < 0; ++y) {
        if (menuText.section('\n', y, y).contains("Move to Trash"))
            trashY = y;
    }
    QVERIFY(trashY > 5);
    runJob([&] { app->mouse({Mouse::Press, 14, trashY}); });
    QVERIFY(!exists(m_left + "/alpha.txt"));
    runJob([this] { keys({Key::control('z')}); });
    QVERIFY(exists(m_left + "/alpha.txt"));
    keys({Key::of(Key::Escape)});
    // On ".." or on empty space: nothing is selected, and the menu is the folder's.
    app->mouse({Mouse::RightPress, 10, 5});
    keys({Key::of(Key::Escape)});
    QCOMPARE(app->selectedPaths(), QStringList{m_left + "/alpha.txt"});
    app->mouse({Mouse::RightPress, 10, 3});
    QVERIFY(app->isContextMenuOpen());
    QVERIFY(app->selectedPaths().isEmpty());
    QVERIFY(app->menuNames().contains("New Folder…") && !app->menuNames().contains("Move to Trash"));
    keys({Key::of(Key::Escape)});
    app->mouse({Mouse::RightPress, 10, 15});
    QVERIFY(app->isContextMenuOpen());
    QVERIFY(app->menuNames().contains("Select All"));
    keys({Key::of(Key::Escape)});
    QVERIFY(app->selectedPaths().isEmpty());
    // ".." is no target: with the cursor on it the commands do nothing.
    app->mouse({Mouse::Press, 10, 3});
    keys({Key::of(Key::F8), shiftDelete(), Key::of(Key::F5), Key::of(Key::F2)});
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
    keys({shiftDelete()});
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

    QCOMPARE(app->folder(0), m_left);
}

// A drive that isn't mounted is one of the places: Enter on it mounts it
// (through gio: a script here) and the pane goes there.
void TestTui::unmountedDrive()
{
    const QString tools = m_tmp->path() + "/pretend";
    QDir().mkpath(tools);
    const QString mountPoint = QDir::cleanPath(m_tmp->path()) + "/media/PRETEND-ONE";
    {
        QFile script(tools + "/gio");
        QVERIFY(script.open(QIODevice::WriteOnly));
        script.write(QStringLiteral(R"(#!/bin/sh
if [ "$1 $2" = "mount -li" ]; then
printf 'Volume(0): PRETEND-ONE\n  ids:\n   unix-device: \047/dev/argonaut-test1\047\n  can_mount=1\n'
exit 0
fi
if [ "$1 $2 $3" = "mount -d /dev/argonaut-test1" ]; then mkdir -p '%1'; echo "Mounted /dev/argonaut-test1 at %1"; exit 0; fi
exit 2
)").arg(mountPoint).toUtf8());
        script.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    }
    const QByteArray gioBefore = qgetenv("ARGONAUT_GIO_PROGRAM");
    qputenv("ARGONAUT_GIO_PROGRAM", QFile::encodeName(tools + "/gio"));
    qputenv("ARGONAUT_VOLUMES", "1");
    const auto restore = qScopeGuard([gioBefore] {
        qputenv("ARGONAUT_GIO_PROGRAM", gioBefore);
        qunsetenv("ARGONAUT_VOLUMES");
    });
    app = std::make_unique<App>(m_left, QString());
    app->setSidebarShown(true);
    settle();
    QTRY_VERIFY_WITH_TIMEOUT(screen(110, 40).contains("PRETEND-ONE"), 10000);
    // From the command palette, as any place.
    keys({Key::control('p')});
    type("go to pretend-one");
    QCOMPARE(app->paletteNames().first(), QStringLiteral("Go to PRETEND-ONE"));
    keys({Key::of(Key::Enter)});
    QVERIFY2(app->message().contains("Mounting"), qPrintable(app->message()));
    QTRY_COMPARE_WITH_TIMEOUT(app->folder(0), mountPoint, 10000);
}

// Ctrl+P: the command palette. Type a few letters, Enter does it; a command
// is the same as its key, so it asks what its key asks.
void TestTui::commandPalette()
{
    QVERIFY(screen(140, 30).contains("^P Commands"));
    keys({Key::control('p')});
    QVERIFY(app->isPaletteOpen());
    QString text = screen(100, 30);
    QVERIFY2(text.contains("Commands") && text.contains("> "), qPrintable(text));
    QVERIFY(text.contains("Go to Folder…") && text.contains("New Folder…"));
    QVERIFY(text.contains("Type to find a command"));
    QStringList names = app->paletteNames();
    QVERIFY(names.contains("Copy to the Other Pane")); // two panes here
    QVERIFY(names.contains("Go to Trash") && names.contains("Quit Argonaut"));
    QVERIFY(!names.contains("Go Back") && !names.contains("View as List")); // nowhere to go back to; it is a list already
    // The keys are the palette's: "q" is a letter, nothing happens to the files.
    QSignalSpy quit(app.get(), &App::quitRequested);
    type("q");
    QCOMPARE(quit.size(), 0);
    QCOMPARE(app->paletteNames().first(), QStringLiteral("Quit Argonaut"));
    keys({Key::of(Key::Backspace)});

    // A few letters find it: the start of a word, letters in order, accents and case aside.
    type("new");
    QCOMPARE(app->paletteNames().first(), QStringLiteral("New Folder…"));
    keys({Key::control('u')});
    type("vg");
    QCOMPARE(app->paletteNames().first(), QStringLiteral("View as Grid"));
    keys({Key::control('u')});
    type("zzzz");
    QVERIFY(app->paletteNames().isEmpty());
    QVERIFY(screen(100, 30).contains("No command fits that"));
    keys({Key::of(Key::Enter)}); // nothing to do: still open
    QVERIFY(app->isPaletteOpen());
    keys({Key::of(Key::Escape)});
    QVERIFY(!app->isPaletteOpen());

    // Enter does it. A view by name; a place by name.
    keys({Key::control('p')});
    type("big");
    keys({Key::of(Key::Enter)});
    QVERIFY(!app->isPaletteOpen());
    QCOMPARE(app->view(0), App::View::Icons);
    keys({Key::control('p')});
    QVERIFY(app->paletteNames().contains("View as List") && !app->paletteNames().contains("View as Big Icons"));
    type("list");
    keys({Key::of(Key::Enter)});
    QCOMPARE(app->view(0), App::View::List);
    keys({Key::control('p')});
    type("trash");
    QVERIFY(app->paletteNames().contains("Go to Trash"));
    keys({Key::of(Key::Down), Key::of(Key::Up)});
    const int trashRow = int(app->paletteNames().indexOf("Go to Trash"));
    for (int i = 0; i < trashRow; ++i)
        keys({Key::of(Key::Down)});
    keys({Key::of(Key::Enter)});
    settle();
    QCOMPARE(app->folder(0), Trash::homeFilesDir());
    keys({Key::control('p')});
    QVERIFY(app->paletteNames().contains("Go Back"));
    type("back");
    keys({Key::of(Key::Enter)});
    settle();
    QCOMPARE(app->folder(0), m_left);

    // A command that asks, asks: deleting for good still needs its "y".
    app->selectPath(m_left + "/alpha.txt");
    keys({Key::control('p')});
    type("delete perm");
    QCOMPARE(app->paletteNames().first(), QStringLiteral("Delete Permanently…"));
    keys({Key::of(Key::Enter)});
    QVERIFY(!app->isPaletteOpen());
    QVERIFY(app->isAsking());
    keys({Key::of(Key::Enter)}); // not "y"
    settle();
    QVERIFY(exists(m_left + "/alpha.txt"));
    // Trash through the palette, then undo through it.
    keys({Key::control('p')});
    type("move to trash");
    runJob([this] { keys({Key::of(Key::Enter)}); });
    QVERIFY(!exists(m_left + "/alpha.txt"));
    keys({Key::control('p')});
    type("undo");
    QVERIFY(app->paletteNames().first().startsWith("Undo"));
    runJob([this] { keys({Key::of(Key::Enter)}); });
    QVERIFY(exists(m_left + "/alpha.txt"));

    // The mouse: a click on a command does it, one beside the palette closes it.
    keys({Key::control('p')});
    type("hidden");
    Screen s(100, 30);
    app->render(s);
    int row = -1;
    for (int y = 0; y < 30; ++y) {
        if (s.line(y).contains("Show Hidden Files"))
            row = y;
    }
    QVERIFY(row > 0);
    QVERIFY(!screen(100, 30).contains(".secret"));
    app->mouse({Tui::Mouse::Press, 40, row});
    settle();
    QVERIFY(!app->isPaletteOpen());
    QVERIFY(screen(100, 30).contains(".secret"));
    keys({Key::control('p')});
    app->mouse({Tui::Mouse::Press, 2, 20});
    QVERIFY(!app->isPaletteOpen());
    // The key line's "^P Commands" opens it too.
    app->render(s);
    const int at = int(s.line(29).indexOf("^P"));
    QVERIFY(at > 0);
    app->mouse({Tui::Mouse::Press, at + 1, 29});
    QVERIFY(app->isPaletteOpen());
    keys({Key::control('p')}); // again: closed
    QVERIFY(!app->isPaletteOpen());
}

// What an extension shows (the System Dashboard) is in the preview here
// too: its command is in the palette, it is told this is the terminal and
// how many characters there is room for, its text keeps its colours, and it
// hears when the result is no longer shown.
void TestTui::extensionResultInThePreview()
{
    if (QStandardPaths::findExecutable("python3").isEmpty())
        QSKIP("python3 isn't installed (the test extension is a Python script)");
    Extensions::Host *host = Extensions::Host::instance();
    const QString fakeFolder = QString::fromLocal8Bit(qgetenv("ARGONAUT_EXTENSIONS_PATH")) + "/fake";
    QFile::remove(fakeFolder + "/results");
    auto logged = [&] {
        QFile file(fakeFolder + "/results");
        return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()) : QString();
    };
    host->setEnabled("fake", true);
    app = std::make_unique<App>(m_left, QString());
    settle();
    QCOMPARE(host->interfaceName(), QStringLiteral("terminal"));
    // Its command for the window's View menu is in the palette here.
    QTRY_VERIFY_WITH_TIMEOUT([&] {
        keys({Key::control('p')});
        const bool there = app->paletteNames().contains("In The & Menu");
        keys({Key::of(Key::Escape)});
        return there;
    }(), 10000);
    // Its commands for a right click are in the right click's menu, those
    // that fit the item, and go out with it; one that asks first, asks.
    screen(100, 30);
    app->mouse({Tui::Mouse::RightPress, 10, 5});
    QVERIFY(app->isContextMenuOpen());
    QCOMPARE(app->selectedPaths(), QStringList{m_left + "/alpha.txt"});
    QStringList offered = app->menuNames();
    QVERIFY2(offered.contains("Fake: Say Hello") && offered.contains("Risky…"), qPrintable(offered.join('|')));
    QVERIFY2(!offered.contains("Folders Only") && !offered.contains("Here") && !offered.contains("In The & Menu"), qPrintable(offered.join('|')));
    keys({Key::character('r'), Key::character('r'), Key::of(Key::Enter)}); // Rename…, then Risky…
    QVERIFY(app->isAsking() && !app->isContextMenuOpen());
    QVERIFY(screen(100, 30).contains("Really do the risky thing?"));
    keys({Key::of(Key::Enter)}); // not "y"
    QTest::qWait(400);
    QVERIFY(!app->message().contains("invoked"));
    app->mouse({Tui::Mouse::RightPress, 10, 5});
    keys({Key::character('f'), Key::of(Key::Enter)});
    QTRY_VERIFY_WITH_TIMEOUT(app->message().contains("invoked hello on 1: alpha.txt"), 10000);
    // On empty space: the folder's commands, each with the group it is of.
    app->mouse({Tui::Mouse::RightPress, 10, 15});
    offered = app->menuNames();
    QVERIFY2(offered.contains("Here") && offered.contains("Fake: Show Me") && !offered.contains("Risky…"), qPrintable(offered.join('|')));
    keys({Key::of(Key::Escape)});
    QVERIFY(app->selectedPaths().isEmpty());

    QVERIFY(!app->isPreviewShown());
    keys({Key::control('p')});
    type("in the");
    QCOMPARE(app->paletteNames().first(), QStringLiteral("In The & Menu"));
    keys({Key::of(Key::Enter)});

    // The preview opens for it; told the room in characters, it sends a
    // version made for it, which replaces the first where it is.
    QTRY_VERIFY_WITH_TIMEOUT(app->isPreviewShown() && app->resultId() == "t1", 10000);
    QTRY_VERIFY_WITH_TIMEOUT(screen(120, 30).contains("updated cols="), 10000);
    Screen s(120, 30);
    app->render(s);
    QStringList lines = s.text().split('\n');
    const int edge = int(lines.at(5).lastIndexOf(QChar(0x2502)));
    QVERIFY(lines.at(1).mid(edge).contains("In Menu"));
    QVERIFY(lines.at(2).mid(edge).contains("made again"));
    const QRegularExpressionMatch room = QRegularExpression("cols=(\\d+) rows=(\\d+)").match(lines.at(4));
    QVERIFY2(room.hasMatch(), qPrintable(lines.at(4)));
    QVERIFY2(room.captured(1).toInt() >= 30 && room.captured(1).toInt() < 60, qPrintable(lines.at(4))); // the pane's, not the screen's
    QVERIFY(room.captured(2).toInt() >= 10);
    // Its colours are its own; what isn't a colour code is dropped, not obeyed.
    const int at = int(lines.at(4).indexOf("updated"));
    QVERIFY(s.at(at, 4).style.raw);
    QCOMPARE(int(s.at(at, 4).style.fg), 2);
    const int hot = int(lines.at(4).indexOf("hot"));
    QVERIFY(hot > at);
    QCOMPARE(int(s.at(hot, 4).style.fg), 196);
    QVERIFY(s.at(hot, 4).style.bold);
    QVERIFY(!lines.at(4).contains("title") && !lines.at(4).contains("2J"));
    QVERIFY(lines.at(4).mid(edge).contains("rows=")); // and the files beside it are still there
    QVERIFY(lines.at(4).left(edge).contains("Documents/"));
    QCOMPARE(logged(), QStringLiteral("t1 shown\n"));

    // Longer than the pane: it scrolls like any preview.
    QVERIFY(screen(120, 30).contains("row 5") && !screen(120, 30).contains("row 59"));
    keys({Key::of(Key::Tab)});
    QVERIFY(app->isInPreview());
    keys({Key::of(Key::End)});
    QVERIFY(screen(120, 30).contains("row 59"));
    keys({Key::of(Key::Escape)});

    // The cursor on another item: the file's preview takes the pane back, and the extension is told.
    keys({Key::of(Key::Down)});
    QTRY_VERIFY_WITH_TIMEOUT((screen(120, 30), app->resultId().isEmpty()), 10000);
    QTRY_VERIFY_WITH_TIMEOUT(logged().endsWith("t1 hidden\n"), 10000);
    QTRY_VERIFY_WITH_TIMEOUT(!screen(120, 30).contains("updated cols="), 10000);

    // Shown again, then F3 closes the preview: told again.
    keys({Key::control('p')});
    type("in the");
    keys({Key::of(Key::Enter)});
    QTRY_VERIFY_WITH_TIMEOUT(screen(120, 30).contains("updated cols="), 10000);
    keys({Key::of(Key::F3)});
    QVERIFY(!app->isPreviewShown());
    QTRY_VERIFY_WITH_TIMEOUT(logged().endsWith("t1 shown\nt1 hidden\n"), 10000);

    // A result that can be searched: "/" in the preview asks for the text,
    // which goes to the extension; nothing typed means everything again.
    for (const Extensions::Command &c : host->commands(m_left, {})) {
        if (c.id == "live")
            host->invoke(c, m_left, {});
    }
    QTRY_VERIFY_WITH_TIMEOUT(app->resultId() == "live1", 10000);
    QTRY_VERIFY_WITH_TIMEOUT(screen(120, 30).contains("/ search"), 10000);
    keys({Key::character('/')}); // from the files: not the preview's key
    QVERIFY(!app->isAsking());
    keys({Key::of(Key::Tab)});
    QVERIFY(app->isInPreview());
    keys({Key::character('/')});
    QVERIFY(app->isAsking());
    QVERIFY(screen(120, 30).contains("Search the fake"));
    type("abc");
    keys({Key::of(Key::Enter)});
    QTRY_VERIFY_WITH_TIMEOUT(screen(120, 30).contains("searched: abc"), 10000);
    QCOMPARE(app->resultId(), QStringLiteral("live1"));
    keys({Key::character('/')}); // what was searched for is there to change
    QVERIFY(screen(120, 30).contains("abc"));
    keys({Key::of(Key::Backspace), Key::of(Key::Backspace), Key::of(Key::Backspace), Key::of(Key::Enter)});
    QTRY_VERIFY2_WITH_TIMEOUT(!screen(120, 30).contains("searched: abc") && screen(120, 30).contains("searched:"), qPrintable(screen(120, 30)), 3000);
    keys({Key::character('/'), Key::of(Key::Escape)}); // Esc: nothing is asked
    QVERIFY(!app->isAsking());
    host->setEnabled("fake", false);
}

// The menus of the top line: File, Edit, View, Go, Help. Alt and a letter
// or a click opens one; an item is the same as its key, so it asks what its
// key asks; what can't be done now is there, dim, and can't be chosen.
void TestTui::menus()
{
    QString text = screen(100, 30);
    const QString top = text.section('\n', 0, 0);
    for (const char *title : {"Argonaut", " File ", " Edit ", " View ", " Go ", " Help"})
        QVERIFY2(top.contains(QLatin1String(title)), qPrintable(top));
    auto alt = [](char32_t c) {
        Key key = Key::character(c);
        key.alt = true;
        return key;
    };
    keys({alt('f')});
    QCOMPARE(app->menuTitle(), QStringLiteral("File"));
    QVERIFY(app->isAsking()); // the files' keys aren't the files' now
    text = screen(100, 30);
    for (const char *part : {"New Folder…", "F7", "Rename…", "Move to Trash", "Delete Permanently…", "Quit"})
        QVERIFY2(text.contains(QString::fromUtf8(part)), part);
    QVERIFY(!text.contains("Tab")); // no tabs in a terminal
    // Two panes: copying to the other can be chosen. One: it can't, and the arrows pass it.
    QVERIFY(app->menuNames().contains("Copy to the Other Pane"));
    keys({Key::of(Key::Escape), Key::of(Key::F4), alt('f')});
    QVERIFY(!app->isSplit());
    QVERIFY(app->menuNames().contains("(Copy to the Other Pane)"));
    QVERIFY(app->menuNames().contains("New Folder…"));
    for (int i = 0; i < 20; ++i) // round and round, over the lines and what can't be chosen
        keys({Key::of(Key::Down)});
    // Esc closes it and nothing happened.
    keys({Key::of(Key::Escape)});
    QVERIFY(app->menuTitle().isEmpty());
    QVERIFY(!app->isAsking());
    QVERIFY(exists(m_left + "/alpha.txt"));

    // Right and Left go to the next menu, round and round.
    keys({alt('f'), Key::of(Key::Right)});
    QCOMPARE(app->menuTitle(), QStringLiteral("Edit"));
    QVERIFY(app->menuNames().contains("(Undo)")); // nothing to undo yet
    keys({Key::of(Key::Left), Key::of(Key::Left)});
    QCOMPARE(app->menuTitle(), QStringLiteral("Help"));
    keys({Key::of(Key::Escape)});

    // A command that asks, asks: deleting for good still needs its "y".
    app->selectPath(m_left + "/alpha.txt");
    keys({alt('f'), Key::character('d')}); // a letter: the item that starts with it
    keys({Key::of(Key::Enter)});
    QVERIFY(app->menuTitle().isEmpty());
    QVERIFY(app->isAsking());
    keys({Key::of(Key::Enter)}); // not "y"
    settle();
    QVERIFY(exists(m_left + "/alpha.txt"));
    // Trash from the menu; then Edit has the undo by its name, and it undoes.
    keys({alt('f'), Key::character('m')});
    QVERIFY(screen(100, 30).contains("Move to Trash"));
    runJob([this] { keys({Key::of(Key::Enter)}); });
    QVERIFY(!exists(m_left + "/alpha.txt"));
    keys({alt('e')});
    QVERIFY2(app->menuNames().first().startsWith("Undo ") && !app->menuNames().first().startsWith("("), qPrintable(app->menuNames().join('|')));
    runJob([this] { keys({Key::of(Key::Enter)}); }); // the first item that can be chosen
    QVERIFY(exists(m_left + "/alpha.txt"));

    // The mouse: a click on a title opens it, on an item chooses it.
    text = screen(100, 30);
    const int viewX = int(text.section('\n', 0, 0).indexOf(" View ")) + 1;
    app->mouse({Tui::Mouse::Press, viewX, 0});
    QCOMPARE(app->menuTitle(), QStringLiteral("View"));
    text = screen(100, 30);
    QVERIFY2(text.contains("● List") && text.contains("Big Icons") && text.contains("Hidden Files") && text.contains("Theme…"), qPrintable(text));
    const QStringList lines = text.split('\n');
    int gridY = -1;
    for (int y = 0; y < lines.size(); ++y) {
        if (gridY < 0 && lines.at(y).contains("Grid") && lines.at(y).contains("F9")) // the first: the key line has "F9" too
            gridY = y;
    }
    QVERIFY(gridY > 0);
    app->mouse({Tui::Mouse::Press, viewX + 4, gridY});
    QVERIFY(app->menuTitle().isEmpty());
    QVERIFY(app->isGrid(0));
    // A click beside an open menu closes it and does nothing else.
    const QString before = app->cursorPath();
    app->mouse({Tui::Mouse::Press, viewX, 0});
    QCOMPARE(app->menuTitle(), QStringLiteral("View"));
    QVERIFY(screen(100, 30).contains("● Grid"));
    app->mouse({Tui::Mouse::Press, 80, 20});
    QVERIFY(app->menuTitle().isEmpty());
    QCOMPARE(app->cursorPath(), before);
    // What is on is ticked: hidden files, from the menu.
    QVERIFY(!screen(100, 30).contains(".secret"));
    keys({alt('v'), Key::character('h'), Key::of(Key::Enter)});
    settle();
    QVERIFY(screen(100, 30).contains(".secret"));
    keys({alt('v')});
    QVERIFY(screen(100, 30).contains("✓ Hidden Files"));
    // Theme…: the palette, with the themes to choose from.
    keys({Key::character('t'), Key::character('t'), Key::character('t'), Key::of(Key::Enter)}); // Two Panes, Terminal, Theme…
    QVERIFY(app->isPaletteOpen());
    QVERIFY(app->paletteNames().first().startsWith("Theme: "));
    keys({Key::of(Key::Escape)});

    // Go: the places by name; Help: the keys.
    keys({alt('g')});
    QVERIFY(app->menuNames().contains("Home") && app->menuNames().contains("Trash") && app->menuNames().contains("(Back)"));
    keys({Key::of(Key::Escape), alt('h'), Key::of(Key::Enter)});
    QVERIFY(screen(100, 30).contains("Argonaut in the terminal"));
}

// The effects (on unless switched off): what is under the mouse lights up,
// names are in the colour of their kind, a menu has rounded corners, a
// shadow, and the rest of the screen dimmed under it, a theme carries its
// exact colours. Switched off, it is the plain look again.
void TestTui::effects()
{
    make("photo.jpg", "x");
    make("bundle.zip", "x");
    app = std::make_unique<App>(m_left, QString());
    settle();
    QVERIFY(app->effects());
    auto rowOf = [](const Screen &s, const QString &text) {
        for (int y = 0; y < s.height(); ++y) {
            if (s.line(y).contains(text))
                return y;
        }
        return -1;
    };
    Screen s(100, 30);
    app->render(s);
    QVERIFY(s.wantsMotion()); // the terminal is asked to say where the mouse is
    const int photoY = rowOf(s, "photo.jpg"), zipY = rowOf(s, "bundle.zip"), betaY = rowOf(s, "beta.txt");
    QVERIFY(photoY > 0 && zipY > 0 && betaY > 0);
    const int nameX = int(s.line(photoY).indexOf("photo.jpg"));
    // A name is in the colour of its kind.
    QCOMPARE(s.at(nameX, photoY).style.ink, Tui::Ink::Good);
    QCOMPARE(s.at(nameX, zipY).style.ink, Tui::Ink::Warning);
    QCOMPARE(s.at(nameX, betaY).style.ink, Tui::Ink::Normal);
    // The mouse over a row: it lights up; moved on, the next one does.
    QCOMPARE(s.at(nameX, photoY).style.paper, Tui::Paper::Normal);
    app->mouse({Tui::Mouse::Move, nameX + 2, photoY});
    app->render(s);
    QCOMPARE(s.at(nameX, photoY).style.paper, Tui::Paper::Hover);
    QCOMPARE(s.at(nameX, betaY).style.paper, Tui::Paper::Normal);
    const QString cursorBefore = app->cursorPath();
    app->mouse({Tui::Mouse::Move, nameX + 2, betaY});
    app->render(s);
    QCOMPARE(s.at(nameX, photoY).style.paper, Tui::Paper::Normal);
    QCOMPARE(s.at(nameX, betaY).style.paper, Tui::Paper::Hover);
    QCOMPARE(app->cursorPath(), cursorBefore); // moving the mouse moves nothing else
    QVERIFY(app->selectedPaths().isEmpty());

    // A menu: rounded, with a shadow, over a screen that steps back; the
    // item under the mouse is the chosen one.
    Key alt = Key::character('f');
    alt.alt = true;
    keys({alt});
    app->render(s);
    QString text = s.text();
    QVERIFY2(text.contains("╭") && text.contains("╮") && text.contains("╰") && text.contains("╯") && !text.contains("┌"), qPrintable(text));
    QCOMPARE(s.at(nameX + 60, photoY).style.ink, Tui::Ink::Dim); // beside the menu: dimmed
    bool shadow = false;
    for (int y = 0; y < s.height(); ++y) {
        for (int x = 0; x < s.width(); ++x)
            shadow = shadow || s.at(x, y).style.paper == Tui::Paper::Shadow;
    }
    QVERIFY(shadow);
    const int renameY = rowOf(s, "Rename…");
    QVERIFY(renameY > 0);
    const int renameX = int(s.line(renameY).indexOf("Rename…"));
    QVERIFY(s.at(renameX, renameY).style.paper != Tui::Paper::Cursor);
    app->mouse({Tui::Mouse::Move, renameX + 1, renameY});
    app->render(s);
    QCOMPARE(s.at(renameX, renameY).style.paper, Tui::Paper::Cursor);
    keys({Key::of(Key::Escape)});

    // A theme carries its very colours, for a terminal that can show them.
    app->setTheme("forest");
    app->render(s);
    QVERIFY(s.palette().themed && s.palette().exact);
    QVERIFY(s.palette().paperRgb[int(Tui::Paper::Normal)] != s.palette().paperRgb[int(Tui::Paper::Hover)]);
    QVERIFY(s.palette().paperRgb[int(Tui::Paper::Normal)] != s.palette().paperRgb[int(Tui::Paper::Shadow)]);
    QVERIFY(s.palette().inkRgb[int(Tui::Ink::Normal)] != s.palette().paperRgb[int(Tui::Paper::Normal)]);

    // Off, from the View menu's own item: the plain look.
    keys({Key::control('p')});
    type("effects off");
    QCOMPARE(app->paletteNames().first(), QStringLiteral("Effects Off: The Plain Look"));
    keys({Key::of(Key::Enter)});
    QVERIFY(!app->effects());
    app->render(s);
    QVERIFY(!s.wantsMotion());
    QVERIFY(s.palette().themed && !s.palette().exact);
    QCOMPARE(s.at(nameX, photoY).style.ink, Tui::Ink::Normal);
    app->mouse({Tui::Mouse::Move, nameX + 2, photoY});
    app->render(s);
    QCOMPARE(s.at(nameX, photoY).style.paper, Tui::Paper::Normal);
    keys({alt});
    app->render(s);
    text = s.text();
    QVERIFY2(text.contains("┌") && !text.contains("╭"), qPrintable(text));
    QCOMPARE(s.at(nameX + 60, photoY).style.paper, Tui::Paper::Normal);
    keys({Key::of(Key::Escape)});
    // And it is remembered.
    app = std::make_unique<App>(m_left, QString());
    QVERIFY(!app->effects());
    app->setEffects(true);
    app->setTheme("terminal");
}

// Typing goes to a name, as in the window: what is typed is the start of
// the name (else something inside it), the same letter again goes through
// the names that start with it, and a pause begins a new name. So a plain
// letter is never a command: q doesn't quit, D doesn't delete.
void TestTui::typingGoesToAName()
{
    make("delta.txt");
    make("doc 2.txt");
    make("Ωmega.txt");
    app = std::make_unique<App>(m_left, QString());
    settle();
    QSignalSpy quit(app.get(), &App::quitRequested);
    auto name = [this] { return QFileInfo(app->cursorPath()).fileName(); };
    auto pause = [] { QTest::qWait(1100); }; // longer than a name is waited for

    type("b");
    QCOMPARE(name(), QStringLiteral("beta.txt"));
    QCOMPARE(app->typedName(), QStringLiteral("b"));
    QVERIFY2(screen().contains("Typed: b"), qPrintable(screen()));
    // Going on: every character is part of the name, a dot too (it doesn't show the hidden files).
    type("eta.t");
    QCOMPARE(name(), QStringLiteral("beta.txt"));
    QVERIFY(!screen().contains(".secret"));
    pause();

    // The start of a name, whatever its case; more typed, and it is another one.
    type("DO");
    QCOMPARE(name(), QStringLiteral("Documents"));
    type("c ");
    QCOMPARE(name(), QStringLiteral("doc 2.txt")); // a space is part of a name: nothing was selected
    QVERIFY(app->selectedPaths().isEmpty());
    // Backspace takes a character back (it doesn't go up a folder now).
    keys({Key::of(Key::Backspace), Key::of(Key::Backspace)});
    QCOMPARE(app->typedName(), QStringLiteral("DO"));
    QCOMPARE(app->folder(0), m_left);
    pause();

    // The same letter again and again: through the names that start with it, round and round.
    keys({Key::of(Key::Home)});
    QStringList seen;
    for (int i = 0; i < 4; ++i) {
        type("d");
        seen << name();
    }
    QCOMPARE(seen, (QStringList{"Documents", "delta.txt", "doc 2.txt", "Documents"}));
    pause();

    // No name starts with it: one that has it inside. Accents and scripts don't matter.
    type("lph");
    QCOMPARE(name(), QStringLiteral("alpha.txt"));
    pause();
    type("ω");
    QCOMPARE(name(), QStringLiteral("Ωmega.txt"));
    pause();
    // Nothing has it: the cursor stays, and that is said.
    type("zzq");
    QCOMPARE(name(), QStringLiteral("Ωmega.txt"));
    QVERIFY2(app->message().contains("no name has that"), qPrintable(app->message()));
    // Esc ends it; the next key is its own again.
    keys({Key::of(Key::Escape)});
    QVERIFY(app->typedName().isEmpty());
    keys({Key::of(Key::Home)});
    QVERIFY(app->cursorPath().isEmpty()); // on ".."

    // Letters that were commands are letters: nothing quit, nothing was deleted or asked.
    pause();
    type("q");
    QCOMPARE(quit.size(), 0);
    pause();
    type("D");
    QVERIFY(!app->isAsking());
    QCOMPARE(name(), QStringLiteral("Documents")); // the first that starts with d, whatever the case
    pause();
    type("g");
    QVERIFY(!app->isAsking()); // no "go to" line
    keys({Key::control('q')});
    QCOMPARE(quit.size(), 1);
    keys({Key::of(Key::Escape)});

    // In the grid and with big icons too.
    pause();
    keys({Key::of(Key::F9)});
    type("al");
    QCOMPARE(name(), QStringLiteral("alpha.txt"));
    pause();
    keys({Key::of(Key::F9)});
    type("be");
    QCOMPARE(name(), QStringLiteral("beta.txt"));
}

// Themes: the terminal's own colours unless one of Argonaut's themes is
// chosen in the command palette; then every cell has the theme's colours
// (the nearest of the terminal's 256), the places their own background.
void TestTui::themes()
{
    const QString folder = QDir::cleanPath(m_tmp->path()) + "/themes";
    QDir().mkpath(folder);
    QFile file(folder + "/Deep Sea.json");
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(R"({"name": "Deep Sea", "window": "#0b1d2a", "sidebar": "#12303f", "text": "#e0f0ff", "accent": "#00c2a8"})");
    file.close();
    qputenv("ARGONAUT_THEMES_PATH", QFile::encodeName(folder));
    struct Restore
    {
        ~Restore() { qputenv("ARGONAUT_THEMES_PATH", ""); }
    } restore;
    app = std::make_unique<App>(m_left, QString());
    app->setSidebarShown(true);
    settle();
    QCOMPARE(app->themeKey(), QStringLiteral("terminal"));
    Screen s(100, 30);
    app->render(s);
    QVERIFY(!s.palette().themed);
    QCOMPARE(s.at(2, 3).style.paper, Tui::Paper::Side); // the places have a paper of their own (the same colour, unthemed)

    // The palette offers the window's themes, built in and from files; not
    // "Follow System" (a window's notion), not the one in use.
    keys({Key::control('p')});
    QStringList names = app->paletteNames();
    for (const char *name : {"Theme: Light", "Theme: Dark", "Theme: Midnight Blue", "Theme: Forest", "Theme: Deep Sea"})
        QVERIFY2(names.contains(QLatin1String(name)), name);
    QVERIFY(!names.contains("Theme: Follow System"));
    QVERIFY(!names.contains("Theme: The Terminal's Own Colours"));
    type("theme forest");
    QCOMPARE(app->paletteNames().first(), QStringLiteral("Theme: Forest"));
    keys({Key::of(Key::Enter)});
    QCOMPARE(app->themeKey(), QStringLiteral("forest"));
    app->render(s);
    Tui::Palette forest = s.palette();
    QVERIFY(forest.themed);
    const int window = forest.paper[int(Tui::Paper::Normal)];
    QVERIFY(window >= 16 && window <= 255);
    QVERIFY(forest.ink[int(Tui::Ink::Normal)] != window);                 // text reads on its background
    QVERIFY(forest.paper[int(Tui::Paper::Side)] != window);                // the places stand apart
    QVERIFY(forest.paper[int(Tui::Paper::Cursor)] != window);              // and the cursor row
    QVERIFY(forest.ink[int(Tui::Ink::Accent)] != forest.ink[int(Tui::Ink::Normal)]);
    QVERIFY(forest.ink[int(Tui::Ink::Bad)] != forest.ink[int(Tui::Ink::Good)]);

    // Remembered; and now the way back is offered.
    app = std::make_unique<App>(m_left, QString());
    settle();
    QCOMPARE(app->themeKey(), QStringLiteral("forest"));
    app->render(s);
    QVERIFY(s.palette() == forest);
    keys({Key::control('p')});
    QVERIFY(app->paletteNames().contains("Theme: The Terminal's Own Colours"));
    QVERIFY(!app->paletteNames().contains("Theme: Forest"));
    // A theme from a file: its colours, as the nearest of the 256.
    type("deep sea");
    keys({Key::of(Key::Enter)});
    QCOMPARE(app->themeKey(), QStringLiteral("file:Deep Sea"));
    app->render(s);
    QVERIFY(s.palette().themed);
    QVERIFY(!(s.palette() == forest));
    QCOMPARE(int(s.palette().ink[int(Tui::Ink::Accent)]), 37); // #00c2a8 in the 6×6×6 cube: 0, 3, 3
    // A light one: dark text on a light background.
    app->setTheme("light");
    app->render(s);
    QVERIFY(s.palette().paper[int(Tui::Paper::Normal)] > 250 || s.palette().paper[int(Tui::Paper::Normal)] == 231);
    QVERIFY(s.palette().ink[int(Tui::Ink::Normal)] < 245);

    // Back to the terminal's own; a key that is no theme (any more) is that too.
    keys({Key::control('p')});
    type("terminal's own");
    keys({Key::of(Key::Enter)});
    QCOMPARE(app->themeKey(), QStringLiteral("terminal"));
    app->render(s);
    QVERIFY(!s.palette().themed);
    app->setTheme("file:Gone");
    QCOMPARE(app->themeKey(), QStringLiteral("terminal"));
    app->setTheme("system");
    QCOMPARE(app->themeKey(), QStringLiteral("terminal"));
}

// F3: the preview at the right, of the item under the cursor: a text
// file's lines with a scrollbar, an archive's entries, a rough picture, a
// warning for a program in disguise (never its content).
void TestTui::previewAtTheRight()
{
    QByteArray longText;
    for (int i = 1; i <= 120; ++i)
        longText += "line number " + QByteArray::number(i) + (i == 3 ? " " + QByteArray(200, 'w') : QByteArray()) + "\n";
    const QString text = make("long.txt", longText);
    const QString script = make("holiday.pdf", "#!/bin/sh\necho gotcha\n");
    QImage picture(64, 32, QImage::Format_RGB32);
    picture.fill(QColor(200, 30, 30));
    for (int y = 16; y < 32; ++y) {
        for (int x = 0; x < 64; ++x)
            picture.setPixelColor(x, y, QColor(30, 30, 200));
    }
    const QString image = m_left + "/picture.png";
    QVERIFY(picture.save(image));
    app = std::make_unique<App>(m_left, QString());
    settle();
    auto loaded = [this] {
        screen(120, 30); // a frame asks for what is under the cursor
        return !app->isPreviewLoading();
    };
    QVERIFY(screen(120, 30).contains("F3 Preview"));
    QVERIFY(!app->isPreviewShown());

    // F3: beside the files, of the item under the cursor (a folder first).
    keys({Key::of(Key::F3)});
    QVERIFY(app->isPreviewShown());
    QTRY_VERIFY_WITH_TIMEOUT(loaded(), 10000);
    QStringList lines = screen(120, 30).split('\n');
    const int edge = int(lines.at(5).lastIndexOf(QChar(0x2502)));
    QVERIFY2(edge > 60 && edge < 80, qPrintable(lines.join('\n'))); // about two fifths of the width
    QVERIFY(lines.at(1).mid(edge).contains("Documents"));
    QVERIFY2(lines.join('\n').contains("1 item inside."), qPrintable(lines.join('\n')));
    QVERIFY(lines.at(4).left(edge).contains("Documents/")); // the files are still there, narrower

    // It follows the cursor: a text file's lines, long ones on to the next line.
    app->selectPath(text);
    QTRY_VERIFY_WITH_TIMEOUT(loaded() && screen(120, 30).contains("line number 1"), 10000);
    lines = screen(120, 30).split('\n');
    QVERIFY(lines.at(1).mid(edge).contains("long.txt"));
    QVERIFY(lines.at(2).mid(edge).contains("kB") || lines.at(2).mid(edge).contains("bytes"));
    QCOMPARE(lines.at(4).mid(edge + 2).left(13), QStringLiteral("line number 1"));
    QVERIFY(lines.at(6).mid(edge).contains("line number 3 www"));
    QVERIFY(lines.at(7).mid(edge + 2).startsWith("www")); // wrapped, not cut off
    QVERIFY(lines.join('\n').contains("lines 1–"));
    // A scrollbar at its right edge, the handle at the top.
    Screen s(120, 30);
    app->render(s);
    QCOMPARE(s.at(119, 4).ch, char32_t(0x2588));
    QCOMPARE(s.at(119, 25).ch, char32_t(0x2502));

    // The wheel over it scrolls it (not the files); a click on the bar jumps.
    const QString cursorBefore = app->cursorPath();
    app->mouse({Tui::Mouse::WheelDown, 100, 10});
    QVERIFY(!screen(120, 30).contains("line number 1 "));
    QVERIFY(screen(120, 30).contains("line number 4"));
    QCOMPARE(app->cursorPath(), cursorBefore);
    app->mouse({Tui::Mouse::Press, 119, 26}); // the bar's last line: the end
    QVERIFY(screen(120, 30).contains("line number 120"));
    app->render(s);
    QCOMPARE(s.at(119, 26).ch, char32_t(0x2588));
    app->mouse({Tui::Mouse::Press, 119, 4});
    QVERIFY(screen(120, 30).contains("line number 1 ") || screen(120, 30).contains("line number 1\n"));

    // Tab goes to it: the arrows and the page keys scroll; the files' keys do nothing to the files.
    keys({Key::of(Key::Tab)});
    QVERIFY(app->isInPreview());
    keys({Key::of(Key::PageDown), Key::of(Key::Down)});
    QVERIFY(!screen(120, 30).contains("line number 2 "));
    keys({Key::of(Key::F8), Key::of(Key::Delete), shiftDelete()});
    settle();
    QVERIFY(!app->isAsking());
    QVERIFY(exists(text));
    QCOMPARE(app->cursorPath(), cursorBefore);
    keys({Key::of(Key::End)});
    QVERIFY(screen(120, 30).contains("line number 120"));
    keys({Key::of(Key::Home), Key::of(Key::Escape)});
    QVERIFY(!app->isInPreview());

    // A program dressed as a PDF: a warning, never what is in it.
    app->selectPath(script);
    QTRY_VERIFY_WITH_TIMEOUT(loaded() && screen(120, 30).contains("disguised"), 10000);
    QVERIFY(!screen(120, 30).contains("gotcha"));

    // A picture: rough, in half blocks, its colours the picture's (red above blue).
    app->selectPath(image);
    QTRY_VERIFY_WITH_TIMEOUT(loaded() && screen(120, 30).contains(QChar(0x2580)), 10000);
    QVERIFY(screen(120, 30).contains("64 × 32"));
    app->render(s);
    bool red = false, blue = false;
    for (int y = 4; y < 28; ++y) {
        for (int x = edge + 2; x < 118; ++x) {
            const Tui::Style style = s.at(x, y).style;
            if (!style.raw)
                continue;
            red = red || style.fg == 160 || style.fg == 196 || style.fg == 124;
            blue = blue || style.bg == 20 || style.bg == 21 || style.bg == 19 || style.fg == 20 || style.fg == 21 || style.fg == 19;
        }
    }
    QVERIFY(red);
    QVERIFY(blue);

    // ".." has nothing to preview; a narrow terminal has no room for the pane; F3 closes it.
    keys({Key::of(Key::Home)});
    QTRY_VERIFY_WITH_TIMEOUT(screen(120, 30).contains("Up to the folder this one is in."), 10000);
    QVERIFY(!screen(60, 24).contains("Up to the folder"));
    keys({Key::of(Key::F3)});
    QVERIFY(!app->isPreviewShown());
    QVERIFY(!screen(120, 30).contains("Up to the folder"));
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
    // One extension, the tests' own, off until a test switches it on.
    {
        const QString root = sandbox.path();
        QDir().mkpath(root + "/extensions/fake");
        QFile manifest(root + "/extensions/fake/extension.json");
        if (!manifest.open(QIODevice::WriteOnly))
            return 1;
        manifest.write(R"({"id": "fake", "name": "Fake", "protocol": 1, "exec": ["fake.py"]})");
        manifest.close();
        QFile::copy(QStringLiteral(FAKE_EXTENSION), root + "/extensions/fake/fake.py");
        QFile::setPermissions(root + "/extensions/fake/fake.py", QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        qputenv("ARGONAUT_EXTENSIONS_PATH", QFile::encodeName(root + "/extensions"));
        qputenv("FAKE_SHOWS_IN_MENU", "1");
    }
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName("argonaut-test");
    QCoreApplication::setApplicationName("tui");
    TestTui test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_tui.moc"
