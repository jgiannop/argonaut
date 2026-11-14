// Tests for GitStatus: finding the repository, parsing `git status`,
// file and folder states, live refresh, and refusing repositories whose
// config would make git run programs. Needs git; skipped without it.
// HOME and XDG_CONFIG_HOME are sandboxed, and system config is ignored.

#include "DirectoryModel.h"
#include "GitStatus.h"

#include <QDir>
#include <QFile>
#include <QProcess>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QApplication>
#include <QtTest>

namespace {

void writeFile(const QString &path, const QByteArray &content = "x\n")
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(content);
}

bool git(const QString &dir, const QStringList &args)
{
    QProcess p;
    p.setWorkingDirectory(dir);
    p.start(QStringLiteral("git"), QStringList{"-c", "user.name=Test", "-c", "user.email=t@example.com",
                                               "-c", "init.defaultBranch=main", "-c", "commit.gpgsign=false"}
                                       + args);
    p.waitForFinished(20000);
    if (p.exitCode() != 0)
        qWarning() << "git" << args << p.readAllStandardError();
    return p.exitStatus() == QProcess::NormalExit && p.exitCode() == 0;
}

bool waitReady(GitStatus &status)
{
    QSignalSpy spy(&status, &GitStatus::changed);
    for (int i = 0; i < 100 && !status.ready(); ++i)
        spy.wait(100);
    return status.ready();
}

} // namespace

class TestGit : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void init();
    void cleanup();

    void parsePorcelain();
    void findRoot();
    void statesOfFilesAndFolders();
    void refreshesAfterGitAdd();
    void conflictsAndRenames();
    void blocksRiskyConfig();
    void notARepository();
    void switchedOff();
    void modelRole();

private:
    QTemporaryDir *m_tmp = nullptr;
    QString m_repo;
};

void TestGit::initTestCase()
{
    if (QStandardPaths::findExecutable("git").isEmpty())
        QSKIP("git isn't installed");
}

void TestGit::init()
{
    m_tmp = new QTemporaryDir;
    QVERIFY(m_tmp->isValid());
    m_repo = QDir::cleanPath(m_tmp->path()) + "/repo";
    QDir().mkpath(m_repo);
    QVERIFY(git(m_repo, {"init", "-q"}));
    writeFile(m_repo + "/tracked.txt", "one\n");
    writeFile(m_repo + "/src/main.cpp", "int main() {}\n");
    writeFile(m_repo + "/docs/guide.md", "# Guide\n");
    QVERIFY(git(m_repo, {"add", "."}));
    QVERIFY(git(m_repo, {"commit", "-q", "-m", "first"}));
    GitStatus::setEnabled(true);
}

void TestGit::cleanup()
{
    delete m_tmp;
}

void TestGit::parsePorcelain()
{
    QByteArray out;
    out += "# branch.oid 1234567890abcdef";
    out += '\0';
    out += "# branch.head feature/x";
    out += '\0';
    out += "1 .M N... 100644 100644 100644 aaa bbb src/a file.cpp";
    out += '\0';
    out += "1 A. N... 000000 100644 100644 000 bbb new.txt";
    out += '\0';
    out += "2 R. N... 100644 100644 100644 aaa bbb R100 moved name.txt";
    out += '\0';
    out += "old name.txt";
    out += '\0';
    out += "u UU N... 100644 100644 100644 100644 a b c clash.txt";
    out += '\0';
    out += "? notes/";
    out += '\0';
    out += "? loose.txt";
    out += '\0';
    const GitStatus::Parsed p = GitStatus::parse(out);
    QCOMPARE(p.branch, QStringLiteral("feature/x"));
    QCOMPARE(p.files.value("src/a file.cpp"), GitStatus::Modified); // a space in the name
    QCOMPARE(p.files.value("new.txt"), GitStatus::Added);
    QCOMPARE(p.files.value("moved name.txt"), GitStatus::Modified);
    QVERIFY(!p.files.contains("old name.txt"));
    QCOMPARE(p.files.value("clash.txt"), GitStatus::Conflicted);
    QCOMPARE(p.files.value("notes"), GitStatus::Untracked);
    QVERIFY(p.untrackedDirs.contains("notes"));
    QCOMPARE(p.files.value("loose.txt"), GitStatus::Untracked);
    QCOMPARE(p.changes, 6);

    QByteArray detached = "# branch.oid abcdef1234567";
    detached += '\0';
    detached += "# branch.head (detached)";
    detached += '\0';
    QCOMPARE(GitStatus::parse(detached).branch, QStringLiteral("detached"));
}

void TestGit::findRoot()
{
    QCOMPARE(GitStatus::findRoot(m_repo), m_repo);
    QCOMPARE(GitStatus::findRoot(m_repo + "/src"), m_repo);
    QCOMPARE(GitStatus::findRoot(QDir::cleanPath(m_tmp->path())), QString());
    // A worktree-style ".git" file counts too.
    writeFile(m_tmp->path() + "/wt/.git", "gitdir: ../repo/.git\n");
    QCOMPARE(GitStatus::findRoot(m_tmp->path() + "/wt"), QDir::cleanPath(m_tmp->path()) + "/wt");
}

void TestGit::statesOfFilesAndFolders()
{
    writeFile(m_repo + "/tracked.txt", "two\n");      // modified
    writeFile(m_repo + "/src/extra.cpp");              // untracked, in a tracked folder
    writeFile(m_repo + "/fresh/a.txt");                // a whole untracked folder
    writeFile(m_repo + "/fresh/deep/b.txt");
    writeFile(m_repo + "/staged.txt");
    QVERIFY(git(m_repo, {"add", "staged.txt"}));       // added

    GitStatus status;
    status.setFolder(m_repo + "/src");
    QVERIFY(status.inRepository());
    QCOMPARE(status.root(), m_repo);
    QVERIFY(waitReady(status));
    QCOMPARE(status.branch(), QStringLiteral("main"));
    QCOMPARE(status.changeCount(), 4); // tracked.txt, staged.txt, src/extra.cpp, fresh/
    QCOMPARE(status.state(m_repo + "/tracked.txt"), GitStatus::Modified);
    QCOMPARE(status.state(m_repo + "/staged.txt"), GitStatus::Added);
    QCOMPARE(status.state(m_repo + "/src/extra.cpp"), GitStatus::Untracked);
    QCOMPARE(status.state(m_repo + "/src/main.cpp"), GitStatus::None);
    QCOMPARE(status.state(m_repo + "/src"), GitStatus::Changed);  // something inside
    QCOMPARE(status.state(m_repo + "/docs"), GitStatus::None);
    QCOMPARE(status.state(m_repo + "/fresh"), GitStatus::Untracked);
    QCOMPARE(status.state(m_repo + "/fresh/deep/b.txt"), GitStatus::Untracked); // inside it
    QCOMPARE(status.state(QDir::cleanPath(m_tmp->path()) + "/elsewhere"), GitStatus::None);

    // Another folder of the same repository keeps the status (no new run).
    status.setFolder(m_repo + "/docs");
    QVERIFY(status.ready());
    QCOMPARE(status.state(m_repo + "/tracked.txt"), GitStatus::Modified);
}

void TestGit::refreshesAfterGitAdd()
{
    writeFile(m_repo + "/new.txt");
    GitStatus status;
    status.setFolder(m_repo);
    QVERIFY(waitReady(status));
    QCOMPARE(status.state(m_repo + "/new.txt"), GitStatus::Untracked);
    // `git add` changes only .git/index: the .git folder is watched.
    QVERIFY(git(m_repo, {"add", "new.txt"}));
    QTRY_COMPARE_WITH_TIMEOUT(status.state(m_repo + "/new.txt"), GitStatus::Added, 8000);
    QVERIFY(git(m_repo, {"commit", "-q", "-m", "add"}));
    QTRY_COMPARE_WITH_TIMEOUT(status.state(m_repo + "/new.txt"), GitStatus::None, 8000);
    QVERIFY(git(m_repo, {"checkout", "-q", "-b", "topic"}));
    QTRY_COMPARE_WITH_TIMEOUT(status.branch(), QStringLiteral("topic"), 8000);
}

void TestGit::conflictsAndRenames()
{
    QVERIFY(git(m_repo, {"checkout", "-q", "-b", "other"}));
    writeFile(m_repo + "/tracked.txt", "other\n");
    QVERIFY(git(m_repo, {"commit", "-q", "-am", "other"}));
    QVERIFY(git(m_repo, {"checkout", "-q", "main"}));
    writeFile(m_repo + "/tracked.txt", "main\n");
    QVERIFY(git(m_repo, {"commit", "-q", "-am", "main"}));
    QVERIFY(!git(m_repo, {"merge", "-q", "other"})); // conflicts
    QVERIFY(git(m_repo, {"mv", "docs/guide.md", "docs/manual.md"}));

    GitStatus status;
    status.setFolder(m_repo);
    QVERIFY(waitReady(status));
    QCOMPARE(status.state(m_repo + "/tracked.txt"), GitStatus::Conflicted);
    QCOMPARE(status.state(m_repo + "/docs/manual.md"), GitStatus::Modified);
    QCOMPARE(status.state(m_repo + "/docs"), GitStatus::Changed);
}

void TestGit::blocksRiskyConfig()
{
    // A filter defined by the repository itself would run on `git status`.
    const QString marker = QDir::cleanPath(m_tmp->path()) + "/ran";
    QVERIFY(git(m_repo, {"config", "filter.evil.clean", "touch " + marker + "; cat"}));
    writeFile(m_repo + "/.gitattributes", "*.txt filter=evil\n");
    writeFile(m_repo + "/tracked.txt", "changed\n");
    GitStatus status;
    status.setFolder(m_repo);
    QVERIFY(waitReady(status));
    QVERIFY(status.blocked());
    QCOMPARE(status.state(m_repo + "/tracked.txt"), GitStatus::None);
    QTest::qWait(300);
    QVERIFY2(!QFileInfo::exists(marker), "the repository's filter must never run");

    // fsmonitor too, also through an included file.
    QVERIFY(git(m_repo, {"config", "--unset", "filter.evil.clean"}));
    writeFile(m_repo + "/.git/extra.cfg", "[core]\n\tfsmonitor = touch " + marker.toUtf8() + "\n");
    QVERIFY(git(m_repo, {"config", "include.path", "extra.cfg"}));
    GitStatus again;
    again.setFolder(m_repo);
    QVERIFY(waitReady(again));
    QVERIFY(again.blocked());
    QVERIFY(!QFileInfo::exists(marker));
}

void TestGit::notARepository()
{
    GitStatus status;
    QSignalSpy spy(&status, &GitStatus::changed);
    status.setFolder(QDir::cleanPath(m_tmp->path()));
    QVERIFY(!status.inRepository());
    QCOMPARE(status.state(QDir::cleanPath(m_tmp->path()) + "/repo"), GitStatus::None);
}

void TestGit::switchedOff()
{
    GitStatus::setEnabled(false);
    GitStatus status;
    status.setFolder(m_repo);
    QVERIFY(!status.inRepository());
    GitStatus::setEnabled(true);
}

void TestGit::modelRole()
{
    writeFile(m_repo + "/tracked.txt", "two\n");
    DirectoryModel model;
    QSignalSpy loaded(&model, &DirectoryModel::loaded);
    QSignalSpy gitChanged(&model, &DirectoryModel::gitChanged);
    model.setPath(m_repo);
    QVERIFY(loaded.wait(10000));
    QTRY_VERIFY_WITH_TIMEOUT(model.git()->ready(), 8000);
    const QModelIndex tracked = model.indexForPath(m_repo + "/tracked.txt");
    QCOMPARE(tracked.data(DirectoryModel::GitStatusRole).toInt(), int(GitStatus::Modified));
    QVERIFY(tracked.data(Qt::ToolTipRole).toString().contains("Git: modified"));
    QCOMPARE(model.indexForPath(m_repo + "/src").data(DirectoryModel::GitStatusRole).toInt(), int(GitStatus::None));
    // A change in the open folder refreshes the badges with the listing.
    writeFile(m_repo + "/top.txt");
    QTRY_COMPARE_WITH_TIMEOUT(model.indexForPath(m_repo + "/top.txt").data(DirectoryModel::GitStatusRole).toInt(),
                              int(GitStatus::Untracked), 8000);
    // Deeper changes aren't watched: the window asks again (activation, jobs).
    writeFile(m_repo + "/src/main.cpp", "int main() { return 1; }\n");
    model.refreshGit();
    QTRY_COMPARE_WITH_TIMEOUT(model.indexForPath(m_repo + "/src").data(DirectoryModel::GitStatusRole).toInt(),
                              int(GitStatus::Changed), 8000);
}

int main(int argc, char **argv)
{
    QTemporaryDir sandbox;
    if (!sandbox.isValid())
        return 1;
    qputenv("HOME", QFile::encodeName(sandbox.path() + "/home"));
    qputenv("XDG_CONFIG_HOME", QFile::encodeName(sandbox.path() + "/config"));
    qputenv("XDG_DATA_HOME", QFile::encodeName(sandbox.path() + "/data"));
    qputenv("XDG_CACHE_HOME", QFile::encodeName(sandbox.path() + "/cache"));
    qputenv("GIT_CONFIG_NOSYSTEM", "1");
    qputenv("KOSMOS_TRASH_HOME_ONLY", "1");
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    QDir().mkpath(sandbox.path() + "/home");
    QApplication app(argc, argv);
    TestGit test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_git.moc"
