// What the extensions that draw their own result share
// (extensions/shared/ResultDrawing.h): the list that Running Services and
// Running Processes show, its search, its bars, and the room Argonaut
// reports. Until now these were only tested through those extensions.

#include "TestSupport.h"
#include "shared/ResultDrawing.h"

#include <QApplication>
#include <QRegularExpression>
#include <QtTest>

class TestResultDrawing : public QObject
{
    Q_OBJECT

private slots:
    void namesAreData();
    void searchKeepsWhatFits();
    void barsGrowByEighths();
    void textNeverWiderThanThePane();
    void pictureIsAsLongAsTheList();
    void roomIsReadFromTheMessage();
    void pictureFileIsThisProgramsOwn();

private:
    static QString plain(const QString &ansi)
    {
        static const QRegularExpression codes(QStringLiteral("\x1b\\[[0-9;]*m"));
        return QString(ansi).remove(codes);
    }
    static QList<ListRow> rows()
    {
        return {{"database", {}, "The Database", "200", "3.0 GB", 1.0, false},
                {"Music Player", "user", "plays music", "17", "1.5 GB", 0.5, true},
                {"tiny", {}, "A small one", "2001", "1.0 MB", 0.01, false}};
    }
};

// A name comes from outside (a process can call itself anything): what
// isn't a printable character becomes "?", so it can't colour the terminal
// or break a line.
void TestResultDrawing::namesAreData()
{
    QCOMPARE(plainText("plain name"), QStringLiteral("plain name"));
    QCOMPARE(plainText("Ωmega 日本 é"), QStringLiteral("Ωmega 日本 é"));
    QCOMPARE(plainText("evil\x1b[31mred"), QStringLiteral("evil?[31mred"));
    QCOMPARE(plainText("two\nlines\tand\ra\x7f" "del"), QStringLiteral("two?lines?and?a?del"));
    QCOMPARE(plainText(QString(QChar(0x9b)) + "31m"), QStringLiteral("?31m")); // the one-character form of ESC [
    QCOMPARE(plainText(QString()), QString());

    QList<ListRow> list{{"a\x1b[1m", "m\nk", "ab\tout", "t\x07g", "1 B", 1.0, false}};
    const QString text = drawListAnsi(ListHead(), list, 60);
    // The only escape sequences in it are the drawing's own colour codes.
    QVERIFY(!plain(text).contains(QChar(0x1b)));
    QVERIFY(plain(text).contains("a?[1m"));
}

void TestResultDrawing::searchKeepsWhatFits()
{
    ListHead head;
    head.big = "3";
    head.beside = "all";
    QList<ListRow> list = rows();
    narrowList(&head, &list, QString(), 10); // nothing searched: as it was
    QCOMPARE(int(list.size()), 3);
    QCOMPARE(head.big, QStringLiteral("3"));
    QVERIFY(head.more.isEmpty());

    // By name, whatever the case; the bars are against the biggest that is left.
    narrowList(&head, &list, "  MUSIC ", 10);
    QCOMPARE(int(list.size()), 1);
    QCOMPARE(list.first().name, QStringLiteral("Music Player"));
    QCOMPARE(list.first().share, 1.0);
    QCOMPARE(head.big, QStringLiteral("1"));
    QVERIFY2(head.beside.contains("of 3") && head.beside.contains("MUSIC"), qPrintable(head.beside));

    // By its mark, by what it is, by its tag (a process's number).
    for (const auto &[search, found] : {std::pair{"user", "Music Player"}, std::pair{"small one", "tiny"}, std::pair{"2001", "tiny"},
                                        std::pair{"the data", "database"}}) {
        list = rows();
        head = ListHead();
        narrowList(&head, &list, QString::fromLatin1(search), 10);
        QVERIFY2(list.size() == 1 && list.first().name == QLatin1String(found), search);
    }
    // "200" is one row's whole tag and the start of another's.
    list = rows();
    narrowList(&head, &list, "200", 10);
    QCOMPARE(int(list.size()), 2);

    // Nothing has it: said, with what was searched for (as data).
    list = rows();
    head = ListHead();
    narrowList(&head, &list, "zz\x1b[31m", 10);
    QVERIFY(list.isEmpty());
    QCOMPARE(head.big, QStringLiteral("0"));
    QVERIFY2(head.empty.contains("zz?[31m"), qPrintable(head.empty));

    // More than a picture can hold: the first ones, and how many are left out.
    list = rows();
    head = ListHead();
    narrowList(&head, &list, QString(), 2);
    QCOMPARE(int(list.size()), 2);
    QCOMPARE(list.last().name, QStringLiteral("Music Player"));
    QVERIFY2(head.more.contains("1 more"), qPrintable(head.more));
}

// A bar is always exactly as wide as asked, and what is filled grows with
// the share, an eighth of a cell at a time.
void TestResultDrawing::barsGrowByEighths()
{
    const QChar full(0x2588);
    auto filled = [&](int width, double share) {
        // Eighths in the bar's own colour: the full blocks before the track's
        // begin, and the part block after them.
        const QString bar = blocks(width, share, 6);
        const QString text = plain(bar);
        if (text.size() != width)
            return -1;
        const QString own = bar.left(bar.indexOf(sgr(kTrack)));
        int eighths = 8 * int(plain(own).count(full));
        for (const QChar c : plain(own)) {
            if (c.unicode() >= 0x2589 && c.unicode() <= 0x258f)
                eighths += 0x2590 - c.unicode();
        }
        return eighths;
    };
    QCOMPARE(filled(10, 0.0), 0);
    QCOMPARE(filled(10, 1.0), 80);
    QCOMPARE(filled(10, 0.5), 40);
    QCOMPARE(filled(10, 0.55), 44); // five cells and four eighths of the next
    QCOMPARE(filled(10, 2.0), 80);  // never more than all of it
    QCOMPARE(filled(10, -1.0), 0);  // nor less than none
    int before = -1;
    for (int step = 0; step <= 100; ++step) {
        const int now = filled(12, step / 100.0);
        QVERIFY2(now >= before && now >= 0, qPrintable(QString::number(step)));
        before = now;
    }
    QCOMPARE(before, 96);
    QCOMPARE(int(plain(blocks(1, 0.5, 6)).size()), 1);
}

void TestResultDrawing::textNeverWiderThanThePane()
{
    ListHead head;
    head.title = "RUNNING THINGS";
    head.big = "3";
    head.beside = "2 of one kind · 1 of another kind";
    head.share = 0.4;
    head.line = "Together they use\n4.5 GB of 16.0 GB memory";
    head.listTitle = "BY MEMORY";
    QList<ListRow> list = rows();
    list.append({"a-name-that-is-much-longer-than-any-pane-is-wide-enough-to-hold", "somebody", {}, {}, "999.9 MB", 0.3, false});
    for (int columns : {10, 24, 30, 44, 80, 200, 5000}) {
        const QString text = plain(drawListAnsi(head, list, columns));
        const int most = qBound(24, columns, 200); // what it is drawn for
        for (const QString &line : text.split(QLatin1Char('\n')))
            QVERIFY2(line.size() <= most, qPrintable(QStringLiteral("%1 columns: %2").arg(columns).arg(line)));
        QVERIFY2(text.contains("database") && text.contains("3.0 GB") && text.contains("BY MEMORY"), qPrintable(text));
    }
    // Wide enough: the line under the bar is one line; narrow: cut where it was meant to be.
    QVERIFY(plain(drawListAnsi(head, list, 80)).contains("Together they use 4.5 GB of 16.0 GB memory"));
    const QStringList narrow = plain(drawListAnsi(head, list, 30)).split(QLatin1Char('\n'));
    QVERIFY(narrow.contains("Together they use") && narrow.contains("4.5 GB of 16.0 GB memory"));
    // A mark stays when the name is cut.
    QVERIFY2(plain(drawListAnsi(head, list, 44)).contains("(somebody)"), qPrintable(plain(drawListAnsi(head, list, 44))));
    // No rows: what the head says instead.
    head.empty = "Nothing is running";
    QVERIFY(plain(drawListAnsi(head, {}, 44)).contains("Nothing is running"));
}

void TestResultDrawing::pictureIsAsLongAsTheList()
{
    ListHead head;
    head.title = "RUNNING THINGS";
    head.big = "3";
    const Look look = lookFor(false, QJsonObject{{"window", "#fdf6e3"}, {"surface", "#eee8d5"}, {"accent", "#d33682"}});
    QCOMPARE(look.window, QColor("#fdf6e3"));
    QCOMPARE(look.card, QColor("#eee8d5"));
    const QImage few = drawList(head, rows(), 500, look);
    QCOMPARE(few.width(), 500);
    QCOMPARE(few.pixelColor(1, 1), QColor("#fdf6e3"));
    QList<ListRow> many;
    for (int i = 0; i < 60; ++i)
        many.append({QStringLiteral("row %1").arg(i), {}, {}, QString::number(i), "1 MB", 0.5, false});
    const QImage long60 = drawList(head, many, 500, look);
    QVERIFY2(long60.height() > few.height() * 5, qPrintable(QStringLiteral("%1 against %2").arg(long60.height()).arg(few.height())));
    // Never narrower than can be read, never absurdly wide.
    QCOMPARE(drawList(head, rows(), 10, look).width(), 240);
    QCOMPARE(drawList(head, rows(), 100000, look).width(), 4000);
    // A theme whose panels are the background's colour still gets panels.
    const Look flat = lookFor(true, QJsonObject{{"window", "#202020"}, {"surface", "#202020"}});
    QVERIFY(flat.card != flat.window);
    // The warning colours come before the theme's own.
    QCOMPARE(levelColor(look, 0.5), look.accent);
    QVERIFY(levelColor(look, 0.8) != look.accent);
    QVERIFY(levelColor(look, 0.95) != levelColor(look, 0.8));
}

void TestResultDrawing::roomIsReadFromTheMessage()
{
    Room room;
    QCOMPARE(room.columns, 44);
    QVERIFY(room.width > 0 && room.height > 0);
    room.read(QJsonObject{{"width", 420}, {"height", 800}, {"dark", false}, {"columns", 37}, {"rows", 20},
                          {"colors", QJsonObject{{"window", "#ffffff"}, {"text", "#101010"}, {"accent", "#0000ff"}}},
                          {"font", QString(300, QLatin1Char('f'))}});
    QCOMPARE(room.width, 420);
    QCOMPARE(room.height, 800);
    QCOMPARE(room.columns, 37);
    QCOMPARE(room.look.window, QColor("#ffffff"));
    QCOMPARE(room.look.accent, QColor("#0000ff"));
    QCOMPARE(int(room.look.font.size()), 100); // a name, not a book
    // The window's message has no columns: what the terminal last said stays.
    room.read(QJsonObject{{"width", 300}, {"height", 500}, {"dark", true}});
    QCOMPARE(room.width, 300);
    QCOMPARE(room.columns, 37);
    // A colour that isn't one is not taken.
    room.read(QJsonObject{{"dark", true}, {"colors", QJsonObject{{"window", "not a colour"}, {"accent", "#00ff00"}}}});
    QVERIFY(room.look.window.isValid());
    QCOMPARE(room.look.accent, QColor("#00ff00"));
}

void TestResultDrawing::pictureFileIsThisProgramsOwn()
{
    QTemporaryDir runtime;
    QVERIFY(runtime.isValid());
    qputenv("XDG_RUNTIME_DIR", QFile::encodeName(runtime.path()));
    const QString file = pictureFile("things");
    QVERIFY2(file.startsWith(runtime.path() + "/argonaut-things-") && file.endsWith(".png"), qPrintable(file));
    QVERIFY(file.contains(QString::number(QCoreApplication::applicationPid())));
    QVERIFY(pictureFile("other") != file);
    QVERIFY(save(QImage(10, 10, QImage::Format_RGB32), file));
    QVERIFY(!QImage(file).isNull());
    // No runtime folder (or one that can't be written to): the temporary folder.
    qputenv("XDG_RUNTIME_DIR", "/nonexistent/argonaut-test");
    QVERIFY(pictureFile("things").startsWith(QDir::tempPath() + "/"));
    qunsetenv("XDG_RUNTIME_DIR");
    QVERIFY(pictureFile("things").startsWith(QDir::tempPath() + "/"));
}

int main(int argc, char **argv)
{
    TestSupport::Sandbox sandbox; // before the application object
    if (!sandbox.isValid())
        return 1;
    QApplication app(argc, argv); // drawing text needs it
    TestResultDrawing test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_resultdrawing.moc"
