#include "GitStatus.h"

#include "FileJob.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QProcess>
#include <QSettings>
#include <QStandardPaths>

#if defined(Q_OS_UNIX)
#include <sys/stat.h>
#endif

namespace {

QString gitProgram()
{
    const QString overridden = qEnvironmentVariable("ARGONAUT_GIT_PROGRAM"); // tests
    if (!overridden.isEmpty())
        return overridden;
    static const QString found = QStandardPaths::findExecutable(QStringLiteral("git"));
    return found;
}

// Settings that make `git status` start other programs.
const char *const kRiskyConfig = "^(filter\\..*\\.(clean|smudge|process)|core\\.fsmonitor)$";

void prepare(QProcess *process, const QString &root)
{
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("LC_ALL"), QStringLiteral("C"));
    env.insert(QStringLiteral("GIT_OPTIONAL_LOCKS"), QStringLiteral("0")); // never lock the index
    env.insert(QStringLiteral("GIT_TERMINAL_PROMPT"), QStringLiteral("0"));
    env.remove(QStringLiteral("GIT_DIR"));
    env.remove(QStringLiteral("GIT_WORK_TREE"));
    process->setProcessEnvironment(env);
    process->setWorkingDirectory(root);
    process->setStandardInputFile(QProcess::nullDevice());
}

int severity(GitStatus::State s)
{
    switch (s) {
    case GitStatus::Conflicted:
        return 3;
    case GitStatus::None:
        return 0;
    default:
        return 1;
    }
}

} // namespace

GitStatus::GitStatus(QObject *parent) : QObject(parent)
{
    m_debounce.setSingleShot(true);
    m_debounce.setInterval(500);
    connect(&m_debounce, &QTimer::timeout, this, &GitStatus::start);
}

GitStatus::~GitStatus()
{
    if (m_process) {
        m_process->disconnect(this);
        m_process->kill();
        m_process->waitForFinished(500);
    }
}

bool GitStatus::enabled()
{
    return QSettings().value(QStringLiteral("git/badges"), true).toBool();
}

void GitStatus::setEnabled(bool on)
{
    QSettings().setValue(QStringLiteral("git/badges"), on);
}

QString GitStatus::findRoot(const QString &folder)
{
    if (folder.isEmpty() || !QDir::isAbsolutePath(folder))
        return {};
#if defined(Q_OS_UNIX)
    struct stat start;
    if (::stat(QFile::encodeName(folder).constData(), &start) != 0)
        return {};
#endif
    QString dir = QDir::cleanPath(folder);
    while (true) {
        const QFileInfo git(dir + QStringLiteral("/.git"));
        if (git.isDir() || git.isFile())
            return dir;
        const QString up = QFileInfo(dir).path();
        if (up == dir)
            return {};
#if defined(Q_OS_UNIX)
        struct stat st;
        if (::stat(QFile::encodeName(up).constData(), &st) != 0 || st.st_dev != start.st_dev)
            return {}; // like git: not across drives
#endif
        dir = up;
    }
}

void GitStatus::clear()
{
    const bool had = !m_root.isEmpty() || !m_files.isEmpty();
    m_root.clear();
    m_gitDir.clear();
    m_branch.clear();
    m_changeCount = 0;
    m_blocked = false;
    m_tooSlow = false;
    m_ready = false;
    m_configChecked = false;
    m_files.clear();
    m_folders.clear();
    m_untrackedDirs.clear();
    m_debounce.stop();
    m_again = false;
    if (m_process) {
        m_process->disconnect(this);
        m_process->kill();
        m_process->deleteLater();
        m_process = nullptr;
    }
    if (m_watcher) {
        delete m_watcher;
        m_watcher = nullptr;
    }
    if (had)
        emit changed();
}

void GitStatus::setFolder(const QString &folder)
{
    if (folder == m_folder && (m_root.isEmpty() || folder.startsWith(m_root)))
        return;
    m_folder = folder;
    QString root;
    if (enabled() && !gitProgram().isEmpty() && !folder.isEmpty() && !isNetworkFilesystem(folder))
        root = findRoot(folder);
    if (root == m_root && !root.isEmpty())
        return; // another folder of the same repository: same status
    clear();
    if (root.isEmpty())
        return;
    m_root = root;
    // ".git" is a folder, or (worktrees, submodules) a file "gitdir: <path>".
    const QString dotGit = root + QStringLiteral("/.git");
    if (QFileInfo(dotGit).isFile()) {
        QFile file(dotGit);
        if (file.open(QIODevice::ReadOnly)) {
            const QString line = QString::fromUtf8(file.readLine()).trimmed();
            if (line.startsWith(QLatin1String("gitdir:"))) {
                const QString dir = line.mid(7).trimmed();
                m_gitDir = QDir::cleanPath(QDir::isAbsolutePath(dir) ? dir : root + QLatin1Char('/') + dir);
            }
        }
    } else {
        m_gitDir = dotGit;
    }
    watchGitDir();
    start();
    emit changed(); // in a repository now (branch still unknown)
}

void GitStatus::watchGitDir()
{
    if (m_gitDir.isEmpty())
        return;
    m_watcher = new QFileSystemWatcher(this);
    // The index and HEAD are replaced by renames: watch the folder.
    m_watcher->addPath(m_gitDir);
    connect(m_watcher, &QFileSystemWatcher::directoryChanged, this, [this] { refresh(); });
}

void GitStatus::refresh()
{
    if (!m_root.isEmpty() && !m_blocked && !m_tooSlow)
        m_debounce.start();
}

void GitStatus::start()
{
    if (m_root.isEmpty() || m_blocked || m_tooSlow)
        return;
    if (m_process) {
        m_again = true; // run once more when this one is done
        return;
    }
    if (!m_configChecked)
        checkConfigThenStatus();
    else
        runStatus();
}

// `git config` only reads files: it is safe to run on any repository.
void GitStatus::checkConfigThenStatus()
{
    auto *process = new QProcess(this);
    m_process = process;
    prepare(process, m_root);
    connect(process, &QProcess::finished, this, [this, process](int code, QProcess::ExitStatus status) {
        process->deleteLater();
        m_process = nullptr;
        const QByteArray risky = process->readAllStandardOutput().trimmed();
        m_configChecked = true;
        // Exit 1 = no such setting. Anything else is unusual: stay away.
        if (status != QProcess::NormalExit || (code != 0 && code != 1) || !risky.isEmpty()) {
            m_blocked = true;
            m_ready = true;
            emit changed();
            return;
        }
        runStatus();
    });
    connect(process, &QProcess::errorOccurred, this, [this, process](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart)
            return;
        process->deleteLater();
        m_process = nullptr;
        m_blocked = true;
        emit changed();
    });
    process->start(gitProgram(), {QStringLiteral("config"), QStringLiteral("--local"), QStringLiteral("--includes"),
                                  QStringLiteral("--get-regexp"), QString::fromLatin1(kRiskyConfig)});
}

void GitStatus::runStatus()
{
    auto *process = new QProcess(this);
    m_process = process;
    prepare(process, m_root);
    auto *timeout = new QTimer(process);
    timeout->setSingleShot(true);
    connect(timeout, &QTimer::timeout, this, [this, process] {
        // A huge repository (or a home folder that is one): give up on it.
        process->disconnect(this);
        process->kill();
        process->deleteLater();
        m_process = nullptr;
        m_tooSlow = true;
        m_ready = true;
        emit changed();
    });
    connect(process, &QProcess::finished, this, [this, process](int code, QProcess::ExitStatus status) {
        process->deleteLater();
        m_process = nullptr;
        if (status == QProcess::NormalExit && code == 0)
            finished(process->readAllStandardOutput());
        if (m_again) {
            m_again = false;
            refresh();
        }
    });
    QStringList args;
#if defined(Q_OS_UNIX)
    const QString nice = QStandardPaths::findExecutable(QStringLiteral("nice"));
    if (!nice.isEmpty())
        args << QStringLiteral("-n") << QStringLiteral("10") << gitProgram();
#endif
    args << QStringLiteral("-c") << QStringLiteral("core.fsmonitor=false") << QStringLiteral("--no-optional-locks")
         << QStringLiteral("status") << QStringLiteral("--porcelain=v2") << QStringLiteral("--branch")
         << QStringLiteral("-z") << QStringLiteral("--ignore-submodules=all");
#if defined(Q_OS_UNIX)
    if (!nice.isEmpty()) {
        process->start(nice, args);
        timeout->start(TimeoutMs);
        return;
    }
#endif
    process->start(gitProgram(), args);
    timeout->start(TimeoutMs);
}

GitStatus::Parsed GitStatus::parse(const QByteArray &output)
{
    Parsed parsed;
    const QList<QByteArray> fields = output.split('\0');
    // A path is the rest of the record after a fixed number of fields.
    auto pathAfter = [](const QByteArray &record, int spaces) {
        int at = 0;
        for (int i = 0; i < spaces && at >= 0; ++i)
            at = record.indexOf(' ', at) + 1;
        return at <= 0 ? QString() : QString::fromUtf8(record.mid(at));
    };
    auto fromXY = [](char x, char y) {
        if (x == 'A' && (y == '.' || y == 'M'))
            return y == 'M' ? GitStatus::Modified : GitStatus::Added;
        return GitStatus::Modified;
    };
    for (int i = 0; i < fields.size(); ++i) {
        const QByteArray &rec = fields[i];
        if (rec.isEmpty())
            continue;
        if (rec.startsWith("# branch.head ")) {
            parsed.branch = QString::fromUtf8(rec.mid(14));
        } else if (rec.startsWith("# branch.oid ") && parsed.branch == QLatin1String("(detached)")) {
            parsed.branch = QString::fromUtf8(rec.mid(13, 7));
        } else if (rec.startsWith("1 ") && rec.size() > 4) {
            parsed.files.insert(pathAfter(rec, 8), fromXY(rec[2], rec[3]));
            ++parsed.changes;
        } else if (rec.startsWith("2 ") && rec.size() > 4) {
            parsed.files.insert(pathAfter(rec, 9), GitStatus::Modified);
            ++i; // the original path follows in its own field
            ++parsed.changes;
        } else if (rec.startsWith("u ")) {
            parsed.files.insert(pathAfter(rec, 10), GitStatus::Conflicted);
            ++parsed.changes;
        } else if (rec.startsWith("? ")) {
            QString path = QString::fromUtf8(rec.mid(2));
            if (path.endsWith(QLatin1Char('/'))) {
                path.chop(1);
                parsed.untrackedDirs.insert(path);
            }
            parsed.files.insert(path, GitStatus::Untracked);
            ++parsed.changes;
        }
    }
    if (parsed.branch == QLatin1String("(detached)"))
        parsed.branch = QObject::tr("detached");
    return parsed;
}

void GitStatus::finished(const QByteArray &output)
{
    const Parsed parsed = parse(output);
    m_branch = parsed.branch;
    m_changeCount = parsed.changes;
    m_files.clear();
    m_folders.clear();
    m_untrackedDirs.clear();
    const QString base = m_root + QLatin1Char('/');
    for (auto it = parsed.files.constBegin(); it != parsed.files.constEnd(); ++it) {
        const QString full = base + it.key();
        m_files.insert(full, it.value());
        // Every folder above it (inside the repository) has changes.
        const State folderState = it.value() == Conflicted ? Conflicted : Changed;
        QString dir = QFileInfo(full).path();
        while (dir.size() > m_root.size() && dir.startsWith(base)) {
            const State old = m_folders.value(dir, None);
            if (severity(folderState) <= severity(old) && old != None)
                break; // already marked this way, and everything above it
            m_folders.insert(dir, folderState);
            dir = QFileInfo(dir).path();
        }
    }
    for (const QString &dir : parsed.untrackedDirs)
        m_untrackedDirs.insert(base + dir);
    m_ready = true;
    emit changed();
}

GitStatus::State GitStatus::state(const QString &path) const
{
    if (m_root.isEmpty() || m_blocked || !path.startsWith(m_root))
        return None;
    if (auto it = m_files.constFind(path); it != m_files.constEnd())
        return *it;
    if (auto it = m_folders.constFind(path); it != m_folders.constEnd())
        return *it;
    if (!m_untrackedDirs.isEmpty()) { // inside a folder git lists only as a whole
        QString dir = QFileInfo(path).path();
        while (dir.size() > m_root.size()) {
            if (m_untrackedDirs.contains(dir))
                return Untracked;
            dir = QFileInfo(dir).path();
        }
    }
    return None;
}
