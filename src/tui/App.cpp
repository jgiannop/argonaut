#include "App.h"

#include "DirectoryModel.h"
#include "Extensions.h"
#include "FileJob.h"
#include "FileOperations.h"
#include "Format.h"
#include "Naming.h"
#include "OpenWith.h"
#include "Preview.h"
#include "RecentFiles.h"
#include "ContentType.h"
#include "Starred.h"
#include "Trash.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QPointer>

namespace Tui {

namespace {

const Style kBar{Ink::Normal, Paper::Bar, false};
const Style kDim{Ink::Dim, Paper::Normal, false};

// "~/Documents" or "Trash": a folder as the pane's heading shows it.
QString shown(const QString &path)
{
    if (Trash::isFilesDir(path))
        return QStringLiteral("Trash");
    switch (DirectoryModel::placeOf(path)) {
    case DirectoryModel::Place::Recent:
        return QStringLiteral("Recent");
    case DirectoryModel::Place::Starred:
        return QStringLiteral("Starred");
    case DirectoryModel::Place::Listing: {
        const Extensions::Listing list = Extensions::Host::instance()->listing(path);
        return list.valid ? list.title : QStringLiteral("List");
    }
    case DirectoryModel::Place::Folder:
        break;
    }
    return tildePath(path);
}

QString verb(FileJob::Kind kind, bool done)
{
    switch (kind) {
    case FileJob::Kind::Copy:
        return done ? QStringLiteral("Copied") : QStringLiteral("Copying");
    case FileJob::Kind::Move:
        return done ? QStringLiteral("Moved") : QStringLiteral("Moving");
    case FileJob::Kind::Trash:
        return done ? QStringLiteral("Moved to the Trash") : QStringLiteral("Moving to the Trash");
    case FileJob::Kind::Restore:
        return done ? QStringLiteral("Restored") : QStringLiteral("Restoring");
    case FileJob::Kind::Delete:
        return done ? QStringLiteral("Deleted") : QStringLiteral("Deleting");
    case FileJob::Kind::Extract:
        return done ? QStringLiteral("Extracted") : QStringLiteral("Extracting");
    case FileJob::Kind::Compress:
        return done ? QStringLiteral("Compressed") : QStringLiteral("Compressing");
    }
    return {};
}

} // namespace

App::App(const QString &leftFolder, const QString &rightFolder, QObject *parent)
    : QObject(parent), m_ops(new FileOperations(this))
{
    m_ops->setAskOnConflict(true); // a name that is taken is a question, here too
    connect(m_ops, &FileOperations::jobStarted, this, [this](FileJob *job) {
        connect(job, &FileJob::progressChanged, this, &App::changed);
        emit changed();
    });
    connect(m_ops, &FileOperations::jobFinished, this, &App::jobFinished);
    connect(m_ops, &FileOperations::conflictRaised, this,
            [this](FileJob *job, const QString &source, const QString &existing) {
                m_conflicts.append({job, source, existing});
                if (m_mode == Mode::Normal)
                    nextConflict();
            });
    m_messageTimer.setSingleShot(true);
    m_messageTimer.setInterval(4000);
    connect(&m_messageTimer, &QTimer::timeout, this, [this] {
        m_message.clear();
        emit changed();
    });
    // Git marks and the branch come from the extension, as in the window.
    connect(Extensions::Host::instance(), &Extensions::Host::folderChanged, this, &App::changed);

    const QString folders[2] = {leftFolder, rightFolder};
    for (int i = 0; i < 2; ++i) {
        Pane &p = m_panes[i];
        p.model = new DirectoryModel(this);
        for (auto signal : {&QAbstractItemModel::rowsInserted, &QAbstractItemModel::rowsRemoved})
            connect(p.model, signal, this, [this, i] { rowsChanged(m_panes[i]); });
        connect(p.model, &QAbstractItemModel::modelReset, this, [this, i] { rowsChanged(m_panes[i]); });
        connect(p.model, &QAbstractItemModel::layoutChanged, this, [this, i] { rowsChanged(m_panes[i]); });
        connect(p.model, &QAbstractItemModel::dataChanged, this, &App::changed);
        connect(p.model, &DirectoryModel::loaded, this, [this, i] { rowsChanged(m_panes[i]); });
        connect(p.model, &DirectoryModel::folderGone, this, [this, i] {
            // The open folder went away: up to the nearest one that is left.
            QString up = m_panes[i].current;
            while (!up.isEmpty() && up != QLatin1String("/") && !QFileInfo(up).isDir())
                up = QFileInfo(up).path();
            navigate(m_panes[i], up.isEmpty() ? QDir::homePath() : up, false);
        });
        if (!navigate(p, folders[i], false))
            navigate(p, QDir::homePath(), false);
    }
}

App::~App() = default;

// -------------------------------------------------------------- state ---

QString App::folder(int index) const
{
    return m_panes[qBound(0, index, 1)].current;
}

// A folder that has one above it starts with a ".." row: Enter or a double
// click on it goes up. Places and the Trash have none (Backspace goes back).
int App::upRows(const Pane &p) const
{
    if (p.current.isEmpty() || DirectoryModel::isVirtualPath(p.current) || Trash::isFilesDir(p.current))
        return 0;
    return QFileInfo(p.current).path() != p.current ? 1 : 0;
}

int App::rowCount(const Pane &p) const
{
    return p.model->rowCount() + upRows(p);
}

// The item on a row of the list; empty for the ".." row.
QString App::pathAt(const Pane &p, int row) const
{
    const int item = row - upRows(p);
    if (item < 0 || item >= p.model->rowCount())
        return {};
    return p.model->filePath(p.model->index(item, 0));
}

QString App::cursorPath() const
{
    return pathAt(pane(), pane().cursor);
}

QStringList App::selectedPaths() const
{
    QStringList paths;
    const Pane &p = pane();
    for (int item = 0; item < p.model->rowCount(); ++item) {
        const QString path = p.model->filePath(p.model->index(item, 0));
        if (p.selected.contains(path))
            paths << path;
    }
    return paths;
}

QStringList App::targets() const
{
    const QStringList selected = selectedPaths();
    if (!selected.isEmpty())
        return selected;
    const QString under = cursorPath();
    return under.isEmpty() ? QStringList() : QStringList{under};
}

bool App::canChangeTargets() const
{
    QList<FileRules::Item> items;
    const Pane &p = pane();
    for (const QString &path : targets())
        items.append({path, p.model->indexForPath(path).data(DirectoryModel::OwnerIdRole).toUInt()});
    return FileRules::canChange(items, p.model->isTrash());
}

bool App::isBusy() const
{
    return !m_ops->activeJobs().isEmpty();
}

bool App::isLoading() const
{
    return m_panes[0].model->isLoading() || m_panes[1].model->isLoading();
}

void App::say(const QString &text)
{
    m_message = text;
    m_messageTimer.start();
    emit changed();
}

// --------------------------------------------------------- navigation ---

bool App::navigate(Pane &p, const QString &path, bool remember)
{
    const bool place = DirectoryModel::isVirtualPath(path);
    QString clean = path;
    if (!place) {
        const QFileInfo info(path);
        if (!info.exists() || !info.isDir()) {
            say(QStringLiteral("Folder not found: %1").arg(path));
            return false;
        }
        if (!info.isReadable() || !info.isExecutable()) {
            say(QStringLiteral("You don't have permission to open “%1”").arg(info.fileName()));
            return false;
        }
        clean = QDir::cleanPath(info.absoluteFilePath());
    }
    if (clean == p.current)
        return true;
    if (remember && !p.current.isEmpty())
        p.back.append(p.current);
    // Going up: the cursor lands on the folder we came out of.
    p.wantedCursor = !p.current.isEmpty() && QFileInfo(p.current).path() == clean ? p.current : QString();
    p.current = clean;
    p.selected.clear();
    p.cursor = upRows(p); // on the first item, not on ".." (an empty folder leaves only that)
    p.fresh = true;       // and it stays there while the folder is still arriving
    p.top = 0;
    p.cursorPath.clear();
    p.model->setShowHidden(m_showHidden);
    p.model->setPath(clean);
    emit changed();
    return true;
}

void App::goUp()
{
    Pane &p = pane();
    if (DirectoryModel::isVirtualPath(p.current) || Trash::isFilesDir(p.current)) {
        if (!p.back.isEmpty())
            navigate(p, p.back.takeLast(), false);
        return;
    }
    const QString up = QFileInfo(p.current).path();
    if (up != p.current)
        navigate(p, up);
}

void App::openCursor()
{
    Pane &p = pane();
    if (upRows(p) == 1 && p.cursor == 0) {
        goUp(); // the ".." row
        return;
    }
    const QModelIndex index = p.model->index(p.cursor - upRows(p), 0);
    if (!index.isValid())
        return;
    if (p.model->isDir(index)) {
        if (p.model->isTrash()) {
            say(QStringLiteral("Folders in the Trash can't be opened. Restore it first."));
            return;
        }
        navigate(p, p.model->filePath(index));
    } else if (p.model->isTrash()) {
        say(QStringLiteral("Files in the Trash can't be opened. Restore it first."));
    } else {
        openFile(p.model->filePath(index));
    }
}

namespace {

// What the file really is (its content, not only its name), as the window
// decides it: the same check, the same cache.
ContentType::Result typeOf(const QString &path)
{
    const QFileInfo info(path);
    const qint64 mtime = info.lastModified().toMSecsSinceEpoch();
    ContentType::Result result;
    if (!ContentType::Cache::instance().lookup(path, info.size(), mtime, &result)) {
        result = ContentType::inspect(path);
        ContentType::Cache::instance().store(path, info.size(), mtime, result);
    }
    return result;
}

// Is there a desktop to open an app's window on? Over SSH there isn't.
bool hasDisplay()
{
    return !qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY") || !qEnvironmentVariableIsEmpty("DISPLAY");
}

} // namespace

// Opens a file with the app for what it really is. A program disguised as a
// document is never opened. Where there is no desktop (a server), a text
// file is shown in the terminal instead.
void App::openFile(const QString &path)
{
    const QString name = QFileInfo(path).fileName();
    const ContentType::Result type = typeOf(path);
    if (type.verdict == ContentType::Verdict::Dangerous) {
        say(QStringLiteral("“%1” is a program disguised as a document. Argonaut won't open it.").arg(name));
        return;
    }
    const QString mime = ContentType::effectiveMime(type);
    const bool text = Preview::isTextual(mime);
    if (hasDisplay() && OpenWith::isSupported()) {
        QString defaultId;
        const QList<OpenWith::App> apps = OpenWith::appsFor(mime, &defaultId);
        for (const OpenWith::App &app : apps) {
            if (app.id != defaultId && !defaultId.isEmpty())
                continue;
            if (OpenWith::launch(app, {path})) {
                RecentFiles::add(path, mime);
                say(QStringLiteral("Opening “%1” with %2").arg(name, app.name));
                return;
            }
        }
        if (!text) {
            say(QStringLiteral("No app is set for this kind of file (%1)").arg(mime));
            return;
        }
    }
    if (text) {
        emit viewRequested(path);
        return;
    }
    say(QStringLiteral("There is no desktop here to open “%1” on (text files are shown in the terminal)").arg(name));
}

// F3: a text file, read in the terminal. Never run, whatever it is.
void App::viewFile(const QString &path)
{
    const QFileInfo info(path);
    if (path.isEmpty() || info.isDir()) {
        say(QStringLiteral("F3 shows a text file; this is a folder"));
        return;
    }
    if (!Preview::isTextual(ContentType::effectiveMime(typeOf(path)))) {
        say(QStringLiteral("“%1” isn't a text file").arg(info.fileName()));
        return;
    }
    emit viewRequested(path);
}

void App::moveCursor(Pane &p, int row)
{
    const int count = rowCount(p);
    p.cursor = count == 0 ? 0 : qBound(0, row, count - 1);
    p.cursorPath = pathAt(p, p.cursor);
    p.wantedCursor.clear();
    p.fresh = false; // the user's choice from here on
    emit changed();
}

void App::selectPath(const QString &path)
{
    Pane &p = pane();
    const QModelIndex index = p.model->indexForPath(path);
    if (index.isValid())
        moveCursor(p, index.row() + upRows(p));
    else
        p.wantedCursor = path;
}

// Rows came, went or were sorted: the cursor stays on its item.
void App::rowsChanged(Pane &p)
{
    const int count = rowCount(p);
    const QString wanted = !p.wantedCursor.isEmpty() ? p.wantedCursor : p.cursorPath;
    const QModelIndex index = wanted.isEmpty() ? QModelIndex() : p.model->indexForPath(wanted);
    if (index.isValid()) {
        p.cursor = index.row() + upRows(p);
        p.wantedCursor.clear();
        p.fresh = false;
    } else if (p.fresh) {
        p.cursor = count == 0 ? 0 : qMin(upRows(p), count - 1); // the first item, once there is one
    } else {
        p.cursor = count == 0 ? 0 : qBound(0, p.cursor, count - 1);
    }
    p.cursorPath = pathAt(p, p.cursor);
    emit changed();
}

// ------------------------------------------------------------ commands ---

void App::transfer(bool move)
{
    const QStringList paths = targets();
    if (paths.isEmpty())
        return;
    const QString target = other().current;
    if (!FileRules::isTransferTarget(target)) {
        say(QStringLiteral("The other pane isn't a folder things can be put in"));
        return;
    }
    if (move && !canChangeTargets()) {
        say(QStringLiteral("You can't move items out of this folder"));
        return;
    }
    if (const QString problem = FileRules::transferProblem(paths, target, move); !problem.isEmpty()) {
        say(problem);
        return;
    }
    move ? m_ops->move(paths, target) : m_ops->copy(paths, target);
}

void App::remove(bool permanently)
{
    const QStringList paths = targets();
    if (paths.isEmpty())
        return;
    if (!canChangeTargets()) {
        say(QStringLiteral("You can't change items in this folder"));
        return;
    }
    const FileRules::Removal removal = permanently ? FileRules::Removal::DeleteFromTrash
                                               : FileRules::removalFor(paths, pane().model->isTrash());
    switch (removal) {
    case FileRules::Removal::Trash:
        m_ops->trash(paths);
        break;
    case FileRules::Removal::DeleteFromTrash:
        ask(FileRules::permanentDelete(paths), [this, paths] { m_ops->deletePermanently(paths); });
        break;
    case FileRules::Removal::DeleteNoTrash:
        ask(FileRules::noTrashOnServer(paths), [this, paths] { m_ops->deletePermanently(paths); });
        break;
    }
}

void App::newFolder()
{
    const QString folder = pane().current;
    if (DirectoryModel::isVirtualPath(folder) || Trash::isFilesDir(folder) || !QFileInfo(folder).isWritable()) {
        say(QStringLiteral("You can't create items in this folder"));
        return;
    }
    prompt(QStringLiteral("New folder:"), QString(), 0, [this, folder](const QString &name) {
        if (const QString problem = Naming::nameProblem(name); !problem.isEmpty()) {
            say(problem);
            return;
        }
        const QString path = folder + QLatin1Char('/') + name;
        if (QFileInfo::exists(path) || QFileInfo(path).isSymLink()) {
            say(QStringLiteral("“%1” is already there").arg(name));
            return;
        }
        if (!QDir(folder).mkdir(name)) {
            say(QStringLiteral("Couldn't create the folder"));
            return;
        }
        m_ops->recordUndo(FileJob::Kind::Copy, {{QString(), path}}, QStringLiteral("New Folder"));
        pane().wantedCursor = path;
        pane().model->refresh();
    });
}

void App::rename()
{
    const QString path = cursorPath();
    if (path.isEmpty())
        return;
    if (pane().model->isTrash() || !canChangeTargets()) {
        say(QStringLiteral("You can't rename items in this folder"));
        return;
    }
    const QFileInfo info(path);
    const QString name = info.fileName();
    // The name without its extension is what is usually changed.
    const int base = int(Naming::baseName(name, info.isDir() && !info.isSymLink()).size());
    prompt(QStringLiteral("Rename to:"), name, base, [this, path, name](const QString &typed) {
        if (typed == name)
            return;
        if (const QString problem = Naming::nameProblem(typed); !problem.isEmpty()) {
            say(problem);
            return;
        }
        const QString target = QFileInfo(path).path() + QLatin1Char('/') + typed;
        // Never onto something that is there: rename() would replace it.
        if (QFileInfo::exists(target) || QFileInfo(target).isSymLink()) {
            say(QStringLiteral("“%1” is already there").arg(typed));
            return;
        }
        if (!QFile::rename(path, target)) {
            say(QStringLiteral("Couldn't rename “%1”").arg(name));
            return;
        }
        Starred::instance().renamed(path, target); // a star follows its item
        m_ops->recordUndo(FileJob::Kind::Move, {{path, target}}, QStringLiteral("Rename"));
        pane().selected.remove(path);
        pane().wantedCursor = target;
        pane().model->refresh();
    });
}

void App::goTo()
{
    const QString current = DirectoryModel::isVirtualPath(pane().current) ? QString() : pane().current;
    prompt(QStringLiteral("Go to:"), current, -1, [this](const QString &typed) {
        QString path = typed.trimmed();
        if (path == QLatin1String("~"))
            path = QDir::homePath();
        else if (path.startsWith(QLatin1String("~/")))
            path = QDir::homePath() + path.mid(1);
        else if (path.compare(QLatin1String("trash"), Qt::CaseInsensitive) == 0)
            path = Trash::homeFilesDir();
        else if (!path.isEmpty() && QDir::isRelativePath(path) && !DirectoryModel::isVirtualPath(pane().current))
            path = pane().current + QLatin1Char('/') + path;
        if (!path.isEmpty())
            navigate(pane(), path);
    });
}

void App::toggleHidden()
{
    m_showHidden = !m_showHidden;
    for (Pane &p : m_panes)
        p.model->setShowHidden(m_showHidden);
    say(m_showHidden ? QStringLiteral("Hidden files are shown") : QStringLiteral("Hidden files are not shown"));
}

void App::quit()
{
    if (isBusy()) {
        // Leaving now would stop a copy or move half way.
        say(QStringLiteral("A file operation is still running. Wait for it, or press Esc to stop it."));
        return;
    }
    emit quitRequested();
}

void App::jobFinished(FileJob *job)
{
    const bool history = job->property(FileOperations::HistoryProperty).isValid();
    const QStringList errors = job->errors();
    const int done = int(job->results().size());
    QString text;
    if (!errors.isEmpty())
        text = errors.size() == 1 ? errors.first() : QStringLiteral("%1 (and %2 more problems)").arg(errors.first()).arg(errors.size() - 1);
    else if (job->isCanceled())
        text = QStringLiteral("Stopped");
    else if (history)
        text = job->property(FileOperations::HistoryProperty).toString() == QLatin1String("undo") ? QStringLiteral("Undone")
                                                                                                    : QStringLiteral("Redone");
    else
        text = QStringLiteral("%1 %2").arg(verb(job->kind(), true), done == 1 ? QStringLiteral("1 item") : QStringLiteral("%1 items").arg(done));
    if (job->kind() == FileJob::Kind::Trash && errors.isEmpty() && !history)
        text += QStringLiteral(" (Ctrl+Z brings them back)");
    // What moved or went away is no longer selected; a copy or move lands
    // under the cursor of the pane it went to.
    for (Pane &p : m_panes) {
        p.selected.clear();
        if (!job->results().isEmpty() && (job->kind() == FileJob::Kind::Copy || job->kind() == FileJob::Kind::Move)
            && QFileInfo(job->results().first().target).path() == p.current && &p != &pane())
            p.wantedCursor = job->results().first().target;
        p.model->refresh();
    }
    Extensions::Host::instance()->refreshAll();
    say(text);
}

// --------------------------------------------- prompts and questions ---

void App::prompt(const QString &label, const QString &text, int selectTo, std::function<void(const QString &)> done)
{
    m_mode = Mode::Prompt;
    m_promptLabel = label;
    m_promptText = text;
    m_promptCursor = selectTo < 0 ? int(text.size()) : qBound(0, selectTo, int(text.size()));
    m_promptDone = std::move(done);
    emit changed();
}

void App::ask(const FileRules::Question &question, std::function<void()> yes)
{
    m_mode = Mode::Question;
    m_question = question;
    m_questionYes = std::move(yes);
    emit changed();
}

void App::promptKey(const Key &key)
{
    switch (key.code) {
    case Key::Escape:
        m_mode = Mode::Normal;
        break;
    case Key::Enter: {
        m_mode = Mode::Normal;
        const auto done = std::move(m_promptDone);
        const QString text = m_promptText;
        m_promptDone = nullptr;
        if (done && !text.isEmpty())
            done(text);
        break;
    }
    case Key::Left:
        m_promptCursor = qMax(0, m_promptCursor - 1);
        break;
    case Key::Right:
        m_promptCursor = qMin(int(m_promptText.size()), m_promptCursor + 1);
        break;
    case Key::Home:
        m_promptCursor = 0;
        break;
    case Key::End:
        m_promptCursor = int(m_promptText.size());
        break;
    case Key::Backspace:
        if (m_promptCursor > 0)
            m_promptText.remove(--m_promptCursor, 1);
        break;
    case Key::Delete:
        m_promptText.remove(m_promptCursor, 1);
        break;
    case Key::Char:
        if (key.isControl('u')) { // clear the line, as in a shell
            m_promptText.clear();
            m_promptCursor = 0;
        } else if (!key.ctrl && !key.alt && key.ch >= 0x20) {
            const QString typed = QString::fromUcs4(&key.ch, 1);
            m_promptText.insert(m_promptCursor, typed);
            m_promptCursor += int(typed.size());
        }
        break;
    default:
        break;
    }
    if (m_mode == Mode::Normal && !m_conflicts.isEmpty())
        nextConflict();
    emit changed();
}

void App::questionKey(const Key &key)
{
    // Only a clear "y" goes ahead; anything else is "no".
    const bool yes = key.isChar('y') || key.isChar('Y');
    m_mode = Mode::Normal;
    const auto run = std::move(m_questionYes);
    m_questionYes = nullptr;
    if (yes && run)
        run();
    else
        say(QStringLiteral("Nothing was changed"));
    if (m_mode == Mode::Normal && !m_conflicts.isEmpty())
        nextConflict();
    emit changed();
}

void App::nextConflict()
{
    // A job that ended meanwhile takes its questions with it.
    while (!m_conflicts.isEmpty() && !m_ops->activeJobs().contains(m_conflicts.first().job))
        m_conflicts.removeFirst();
    m_mode = m_conflicts.isEmpty() ? Mode::Normal : Mode::Conflict;
    emit changed();
}

void App::conflictKey(const Key &key)
{
    if (m_conflicts.isEmpty()) {
        m_mode = Mode::Normal;
        return;
    }
    if (key.code != Key::Char && key.code != Key::Escape)
        return;
    const Conflict conflict = m_conflicts.first();
    const bool folders = QFileInfo(conflict.source).isDir() && QFileInfo(conflict.existing).isDir()
                         && !QFileInfo(conflict.source).isSymLink() && !QFileInfo(conflict.existing).isSymLink();
    FileJob::Resolution resolution;
    // A capital letter answers for the rest of the job too.
    const char32_t c = key.code == Key::Escape ? U'c' : key.ch;
    resolution.applyToAll = c >= U'A' && c <= U'Z';
    switch (QChar::toLower(c)) {
    case U'r':
        resolution.action = FileJob::ConflictAction::Replace;
        break;
    case U's':
        resolution.action = FileJob::ConflictAction::Skip;
        break;
    case U'k':
        resolution.action = FileJob::ConflictAction::KeepBoth;
        break;
    case U'm':
        if (!folders)
            return;
        resolution.action = FileJob::ConflictAction::Merge;
        break;
    case U'c':
        resolution.action = FileJob::ConflictAction::Cancel;
        break;
    default:
        return; // not an answer: the question stays
    }
    m_conflicts.removeFirst();
    if (m_ops->activeJobs().contains(conflict.job))
        conflict.job->resolveConflict(resolution);
    nextConflict();
}

// ---------------------------------------------------------------- keys ---

void App::key(const Key &key)
{
    switch (m_mode) {
    case Mode::Prompt:
        promptKey(key);
        return;
    case Mode::Question:
        questionKey(key);
        return;
    case Mode::Conflict:
        conflictKey(key);
        return;
    case Mode::Help:
        m_mode = Mode::Normal;
        emit changed();
        return;
    case Mode::Normal:
        normalKey(key);
        return;
    }
}

void App::mouse(const Mouse &mouse)
{
    if (m_mode != Mode::Normal)
        return; // a question is answered with the keyboard, deliberately
    // The key line: a click on "F5 Copy" is F5.
    if (mouse.kind == Mouse::Press && mouse.y == m_keysY) {
        for (const KeySpot &spot : std::as_const(m_keySpots)) {
            if (mouse.x >= spot.from && mouse.x < spot.to) {
                key(spot.key);
                return;
            }
        }
        return;
    }
    for (int i = 0; i < 2; ++i) {
        Pane &p = m_panes[i];
        if (mouse.x < p.x || mouse.x >= p.x + p.width)
            continue;
        if (mouse.kind == Mouse::WheelUp || mouse.kind == Mouse::WheelDown) {
            // The list moves under the pointer; the cursor comes along only
            // when it would leave the screen.
            const int count = rowCount(p);
            p.top = qBound(0, p.top + (mouse.kind == Mouse::WheelUp ? -3 : 3), qMax(0, count - p.rowsShown));
            if (count > 0)
                p.cursor = qBound(p.top, p.cursor, qMin(count - 1, p.top + p.rowsShown - 1));
            p.cursorPath = pathAt(p, p.cursor);
            emit changed();
            return;
        }
        const bool wasActive = m_active == i;
        m_active = i;
        if (mouse.kind == Mouse::Press && mouse.y == p.rowsY - 2) {
            goUp(); // a click on the folder's name in the heading: up
            emit changed();
            return;
        }
        const int row = p.top + (mouse.y - p.rowsY);
        if (mouse.y < p.rowsY || mouse.y >= p.rowsY + p.rowsShown || row >= rowCount(p)) {
            emit changed(); // empty space: only the pane changes
            return;
        }
        moveCursor(p, row);
        const bool upRow = upRows(p) == 1 && row == 0;
        const QString path = upRow ? QStringLiteral("..") : pathAt(p, row);
        if (upRow && mouse.kind == Mouse::RightPress)
            return; // ".." is not something to select
        if (mouse.kind == Mouse::RightPress) {
            if (!p.selected.remove(path))
                p.selected.insert(path);
            emit changed();
            return;
        }
        // The second click on the same item within the usual time opens it.
        const bool twice = wasActive && m_lastClick.isValid() && m_lastClick.elapsed() < 450 && m_lastClickPath == path;
        m_lastClickPath = path;
        m_lastClick.start();
        if (twice) {
            m_lastClick.invalidate();
            openCursor();
        }
        return;
    }
}

void App::normalKey(const Key &key)
{
    Pane &p = pane();
    switch (key.code) {
    case Key::Up:
        moveCursor(p, p.cursor - 1);
        return;
    case Key::Down:
        moveCursor(p, p.cursor + 1);
        return;
    case Key::PageUp:
        moveCursor(p, p.cursor - qMax(1, p.rowsShown - 1));
        return;
    case Key::PageDown:
        moveCursor(p, p.cursor + qMax(1, p.rowsShown - 1));
        return;
    case Key::Home:
        moveCursor(p, 0);
        return;
    case Key::End:
        moveCursor(p, rowCount(p) - 1);
        return;
    case Key::Enter:
    case Key::Right:
        openCursor();
        return;
    case Key::Left:
        if (key.alt && !p.back.isEmpty())
            navigate(p, p.back.takeLast(), false);
        else
            goUp();
        return;
    case Key::Backspace:
        goUp();
        return;
    case Key::Tab:
        m_active = 1 - m_active;
        emit changed();
        return;
    case Key::Insert: { // select and step down, as in every two-pane manager
        const QString path = cursorPath();
        if (!path.isEmpty()) {
            if (!p.selected.remove(path))
                p.selected.insert(path);
        }
        moveCursor(p, p.cursor + 1);
        return;
    }
    case Key::Delete:
        remove(key.shift);
        return;
    case Key::Escape:
        if (isBusy()) {
            ask({QStringLiteral("Stop"), QStringLiteral("Stop the running file operation?"),
                 QStringLiteral("What is already done stays done."), QStringLiteral("Stop")},
                [this] { m_ops->cancelAll(); });
        } else if (!p.selected.isEmpty()) {
            p.selected.clear();
            emit changed();
        }
        return;
    case Key::F1:
        m_mode = Mode::Help;
        emit changed();
        return;
    case Key::F2:
        rename();
        return;
    case Key::F3:
        viewFile(cursorPath());
        return;
    case Key::F5:
        transfer(false);
        return;
    case Key::F6:
        transfer(true);
        return;
    case Key::F7:
        newFolder();
        return;
    case Key::F8:
        remove(false);
        return;
    case Key::F10:
        quit();
        return;
    case Key::Char:
        break;
    default:
        return;
    }

    if (key.isControl('z')) {
        if (m_ops->canUndo())
            m_ops->undo();
        else
            say(QStringLiteral("Nothing to undo"));
    } else if (key.isControl('y')) {
        if (m_ops->canRedo())
            m_ops->redo();
        else
            say(QStringLiteral("Nothing to redo"));
    } else if (key.isControl('a')) {
        for (int item = 0; item < p.model->rowCount(); ++item)
            p.selected.insert(p.model->filePath(p.model->index(item, 0)));
        emit changed();
    } else if (key.isControl('l') || key.isChar('g')) {
        goTo();
    } else if (key.isControl('r')) {
        p.model->refresh();
        Extensions::Host::instance()->refreshAll();
        say(QStringLiteral("Read again"));
    } else if (key.isControl('q') || key.isChar('q')) {
        quit();
    } else if (key.isChar(' ')) {
        normalKey(Key::of(Key::Insert));
    } else if (key.isChar('.')) {
        toggleHidden();
    } else if (key.isChar('~')) {
        navigate(p, QDir::homePath());
    } else if (key.isChar('D')) {
        remove(true);
    } else if (key.isChar('?')) {
        m_mode = Mode::Help;
        emit changed();
    }
}

// ------------------------------------------------------------- drawing ---

void App::render(Screen &screen)
{
    screen.clear();
    const int w = screen.width();
    const int h = screen.height();
    if (w < 40 || h < 8) {
        screen.put(0, 0, QStringLiteral("The terminal is too small for Argonaut."));
        return;
    }
    if (m_mode == Mode::Help) {
        drawHelp(screen);
        return;
    }
    // The top bar: the program, and what can be undone.
    screen.fill(0, 0, w, 1, kBar);
    screen.put(1, 0, QStringLiteral("Argonaut"), {Ink::Accent, Paper::Bar, true});
    const QString undo = m_ops->canUndo() ? QStringLiteral("Ctrl+Z %1").arg(m_ops->undoText()) : QString();
    screen.put(qMax(11, w - Screen::widthOf(undo) - 1), 0, undo, {Ink::Dim, Paper::Bar, false});

    const int left = (w - 1) / 2;
    const int paneHeight = h - 3; // top bar above; message and key lines below
    drawPane(screen, 0, 0, left, 1, paneHeight);
    for (int y = 1; y < 1 + paneHeight; ++y)
        screen.put(left, y, QStringLiteral("│"), kDim);
    drawPane(screen, 1, left + 1, w - left - 1, 1, paneHeight);
    drawBottom(screen);
}

void App::drawPane(Screen &screen, int index, int x, int width, int top, int height)
{
    Pane &p = m_panes[index];
    const bool active = index == m_active;
    p.x = x;
    p.width = width;
    p.rowsY = top + 2;
    // Heading: the folder, and what an extension says about it (the branch).
    const Style heading{active ? Ink::Accent : Ink::Dim, Paper::Normal, active};
    const Extensions::Info info = p.model->extensionsFolder().isEmpty()
                                      ? Extensions::Info()
                                      : Extensions::Host::instance()->info(p.model->extensionsFolder());
    const int infoWidth = info.text.isEmpty() ? 0 : qMin(Screen::widthOf(info.text), width / 2);
    screen.put(x + 1, top, Screen::elided(shown(p.current), width - 2 - (infoWidth ? infoWidth + 1 : 0)), heading);
    if (infoWidth)
        screen.put(x + width - 1 - infoWidth, top, Screen::elided(info.text, infoWidth, false), kDim);

    // Columns: name, size, when. Narrow panes drop the date, then the size.
    const int dateWidth = width >= 56 ? 17 : 0;
    const int sizeWidth = width >= 34 ? 10 : 0;
    const int nameWidth = width - 2 - (dateWidth ? dateWidth + 1 : 0) - (sizeWidth ? sizeWidth + 1 : 0) - 3;
    const int sizeX = x + 1 + nameWidth + 3 + 1;
    const int dateX = sizeX + sizeWidth + 1;
    screen.put(x + 2, top + 1, p.model->headerData(DirectoryModel::NameColumn, Qt::Horizontal).toString(), kDim, nameWidth);
    if (sizeWidth)
        screen.put(sizeX, top + 1, p.model->headerData(DirectoryModel::SizeColumn, Qt::Horizontal).toString(), kDim, sizeWidth);
    if (dateWidth)
        screen.put(dateX, top + 1, p.model->headerData(DirectoryModel::ModifiedColumn, Qt::Horizontal).toString(), kDim, dateWidth);

    const int rows = qMax(1, height - 3); // heading, column names, footer
    p.rowsShown = rows;
    const int up = upRows(p);
    const int items = p.model->rowCount();
    const int count = items + up;
    p.cursor = count == 0 ? 0 : qBound(0, p.cursor, count - 1);
    if (p.cursor < p.top)
        p.top = p.cursor;
    if (p.cursor >= p.top + rows)
        p.top = p.cursor - rows + 1;
    p.top = qBound(0, p.top, qMax(0, count - rows));

    for (int i = 0; i < rows && p.top + i < count; ++i) {
        const int y = top + 2 + i;
        if (up == 1 && p.top + i == 0) {
            // ".." : up to the folder this one is in.
            const bool cursor = p.cursor == 0;
            Style style{Ink::Folder, cursor ? (active ? Paper::Cursor : Paper::CursorIdle) : Paper::Normal, true};
            screen.fill(x, y, width, 1, style);
            screen.put(x + 2, y, QStringLiteral("../"), style);
            if (sizeWidth) {
                Style detail = style;
                detail.bold = false;
                if (!cursor)
                    detail.ink = Ink::Dim;
                screen.put(sizeX + sizeWidth - 2, y, QStringLiteral("Up"), detail);
            }
            continue;
        }
        const int row = p.top + i - up; // the model's row
        const QModelIndex index0 = p.model->index(row, 0);
        const QString path = p.model->filePath(index0);
        const bool dir = p.model->isDir(index0);
        const bool selected = p.selected.contains(path);
        const bool cursor = row + up == p.cursor;
        Style style;
        style.paper = cursor ? (active ? Paper::Cursor : Paper::CursorIdle) : Paper::Normal;
        style.ink = selected ? Ink::Selected : dir ? Ink::Folder : Ink::Normal;
        style.bold = dir || selected;
        screen.fill(x, y, width, 1, style);
        screen.put(x, y, selected ? QStringLiteral("*") : QStringLiteral(" "), style);
        QString name = p.model->fileName(index0);
        if (dir)
            name += QLatin1Char('/');
        else if (index0.data(DirectoryModel::SymLinkRole).toBool())
            name += QLatin1Char('@');
        screen.put(x + 2, y, Screen::elided(name, nameWidth), style);
        // An extension's mark (Git: M, A, U), in its colour.
        const QVariant mark = index0.data(DirectoryModel::BadgeRole);
        if (mark.isValid()) {
            const Extensions::Badge badge = mark.value<Extensions::Badge>();
            Style markStyle = style;
            markStyle.bold = true;
            markStyle.ink = badge.color == QLatin1String("success") ? Ink::Good
                            : badge.color == QLatin1String("error") ? Ink::Bad
                            : badge.color == QLatin1String("accent") ? Ink::Accent
                            : badge.color == QLatin1String("dim") ? Ink::Dim : Ink::Warning;
            screen.put(x + 2 + nameWidth + 1, y, badge.text.isEmpty() ? QStringLiteral("•") : badge.text, markStyle, 2);
        }
        Style detail = style;
        if (!selected && !cursor)
            detail.ink = Ink::Dim;
        detail.bold = false;
        if (sizeWidth) {
            const QString size = p.model->index(row, DirectoryModel::SizeColumn).data().toString();
            screen.put(sizeX + qMax(0, sizeWidth - Screen::widthOf(size)), y, Screen::elided(size, sizeWidth, false), detail);
        }
        if (dateWidth)
            screen.put(dateX, y, p.model->index(row, DirectoryModel::ModifiedColumn).data().toString(), detail, dateWidth);
    }
    if (items == 0)
        screen.put(x + 2, top + 2 + up, p.model->isLoading() ? QStringLiteral("Loading…") : QStringLiteral("Nothing here"), kDim);

    // Footer: how many, and what is selected.
    QString footer = items == 1 ? QStringLiteral("1 item") : QStringLiteral("%1 items").arg(items);
    if (!p.selected.isEmpty()) {
        qint64 bytes = 0;
        int chosen = 0;
        for (int row = 0; row < items; ++row) {
            const QModelIndex index0 = p.model->index(row, 0);
            if (!p.selected.contains(p.model->filePath(index0)))
                continue;
            ++chosen;
            if (!p.model->isDir(index0))
                bytes += p.model->size(index0);
        }
        footer += QStringLiteral(" · %1 selected").arg(chosen);
        if (bytes > 0)
            footer += QStringLiteral(" (%1)").arg(formatSize(bytes));
    }
    if (count > rows)
        footer += QStringLiteral(" · %1–%2").arg(p.top + 1).arg(qMin(count, p.top + rows));
    screen.put(x + 1, top + height - 1, Screen::elided(footer, width - 2, false), kDim);
}

QString App::statusText() const
{
    if (!m_ops->activeJobs().isEmpty()) {
        const FileJob *job = m_ops->activeJobs().first();
        const FileJob::Progress progress = job->progress();
        QString text = verb(job->kind(), false);
        if (progress.finishing)
            return QStringLiteral("Writing to the drive… don't unplug it yet");
        if (progress.measuring)
            return text + QStringLiteral("… (measuring)");
        if (progress.bytesTotal > 0)
            text += QStringLiteral(" %1%").arg(qBound(qint64(0), progress.bytesDone * 100 / progress.bytesTotal, qint64(100)));
        if (progress.itemsTotal > 1)
            text += QStringLiteral(" · %1 of %2").arg(qMin(progress.itemsDone + 1, progress.itemsTotal)).arg(progress.itemsTotal);
        if (!progress.current.isEmpty())
            text += QStringLiteral(" · %1").arg(progress.current);
        return text + QStringLiteral("   (Esc stops it)");
    }
    return m_message;
}

void App::drawBottom(Screen &screen)
{
    const int w = screen.width();
    const int messageY = screen.height() - 2;
    const int keysY = screen.height() - 1;
    screen.fill(0, keysY, w, 1, kBar);
    m_keySpots.clear();
    m_keysY = keysY;

    if (m_mode == Mode::Prompt) {
        const Style label{Ink::Accent, Paper::Normal, true};
        const int used = screen.put(1, messageY, m_promptLabel, label);
        const int fieldX = 1 + used + 1;
        const int fieldWidth = qMax(4, w - fieldX - 1);
        const Style field{Ink::Normal, Paper::Field, false};
        screen.fill(fieldX, messageY, fieldWidth, 1, field);
        // Keep the cursor in view in a long text.
        const int cursorCells = Screen::widthOf(m_promptText.left(m_promptCursor));
        int skip = 0;
        while (Screen::widthOf(m_promptText.mid(skip, m_promptCursor - skip)) > fieldWidth - 1)
            ++skip;
        screen.put(fieldX, messageY, m_promptText.mid(skip), field, fieldWidth);
        screen.setCursor(fieldX + cursorCells - Screen::widthOf(m_promptText.left(skip)), messageY);
        screen.put(1, keysY, QStringLiteral("Enter: OK   Esc: Cancel"), kBar);
        return;
    }
    if (m_mode == Mode::Question) {
        screen.put(1, messageY, Screen::elided(m_question.text + QLatin1Char(' ') + m_question.detail, w - 2, false),
                   {Ink::Bad, Paper::Normal, true});
        screen.put(1, keysY, QStringLiteral("y: %1   any other key: Cancel").arg(m_question.confirm), kBar);
        return;
    }
    if (m_mode == Mode::Conflict && !m_conflicts.isEmpty()) {
        const Conflict &conflict = m_conflicts.first();
        const bool folders = QFileInfo(conflict.source).isDir() && QFileInfo(conflict.existing).isDir();
        screen.put(1, messageY,
                   Screen::elided(QStringLiteral("“%1” is already in %2.").arg(QFileInfo(conflict.existing).fileName(),
                                                                                tildePath(QFileInfo(conflict.existing).path())),
                                  w - 2, false),
                   {Ink::Warning, Paper::Normal, true});
        screen.put(1, keysY,
                   QStringLiteral("r: Replace (old one to the Trash)   s: Skip   k: Keep both%1   c: Cancel   CAPITAL: for all")
                       .arg(folders ? QStringLiteral("   m: Merge") : QString()),
                   kBar);
        return;
    }
    const QString status = statusText();
    screen.put(1, messageY, Screen::elided(status, w - 2, false), isBusy() ? Style{Ink::Accent, Paper::Normal, false} : Style());
    static const struct
    {
        const char *key;
        const char *what;
        Key::Code code;
    } keys[] = {{"F1", "Help", Key::F1}, {"F2", "Rename", Key::F2}, {"F3", "View", Key::F3}, {"F5", "Copy", Key::F5}, {"F6", "Move", Key::F6},
                {"F7", "New Folder", Key::F7}, {"F8", "Trash", Key::F8}, {"Tab", "Other Pane", Key::Tab},
                {"F10", "Quit", Key::F10}};
    int x = 1;
    for (const auto &entry : keys) {
        const QString key = QString::fromLatin1(entry.key);
        const QString what = QString::fromLatin1(entry.what);
        if (x + key.size() + 1 + what.size() >= w)
            break;
        const int from = x;
        x += screen.put(x, keysY, key, {Ink::Accent, Paper::Bar, true});
        x += screen.put(x, keysY, QLatin1Char(' ') + what, kBar);
        m_keySpots.append({from, x, Key::of(entry.code)}); // clickable
        x += 2;
    }
}

void App::drawHelp(Screen &screen)
{
    static const char *const lines[] = {
        "Argonaut in the terminal",
        "",
        "  Up, Down, PgUp, PgDn, Home, End   move",
        "  Enter, Right                      open the folder, or the file with its app",
        "  F3                                read a text file here in the terminal",
        "  Backspace, Left, Enter on ..      up to the folder it is in",
        "  Alt+Left                          back to where you were",
        "  Tab                               the other pane",
        "  g  or  Ctrl+L                     go to a folder (\"trash\" for the Trash)",
        "  ~                                 home",
        "  .                                 show or hide hidden files",
        "  Ctrl+R                            read the folder again",
        "",
        "  Space, Insert                     select and go down      Ctrl+A   select all",
        "  Esc                               clear the selection (or stop a running operation)",
        "",
        "  F5 / F6                           copy / move to the other pane",
        "  F7                                new folder",
        "  F2                                rename",
        "  F8, Delete                        move to the Trash",
        "  D, Shift+Delete                   delete forever (asks first)",
        "  Ctrl+Z / Ctrl+Y                   undo / redo",
        "",
        "  F10, q                            quit",
        "",
        "  Mouse: click a row to go to it, double-click to open it (.. or the folder's",
        "  name at the top: up), right-click to select, wheel to scroll, click a word on the",
        "  bottom line. Hold Shift to select text.",
        "",
        "Commands act on the selected items; with none selected, on the item under the cursor.",
        "",
        "Press any key.",
    };
    int y = 1;
    for (const char *line : lines) {
        if (y >= screen.height())
            break;
        screen.put(2, y, QString::fromUtf8(line), y == 1 ? Style{Ink::Accent, Paper::Normal, true} : Style());
        ++y;
    }
}

} // namespace Tui
