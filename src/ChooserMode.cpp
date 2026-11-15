#include "ChooserMode.h"

#if defined(ARGONAUT_XCB)
#include <xcb/xcb.h>
#endif

#include "ChooserBar.h"
#include "DirectoryModel.h"
#include "FileChooserPortal.h"

#include <QAction>
#include <QCursor>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QScreen>
#include <QSettings>
#include <QTimer>
#include <QWidget>

using Mode = ChooserRequest::Mode;

ChooserMode::ChooserMode(const ChooserRequest &request, const Host &host, QObject *parent)
    : QObject(parent), m_request(request), m_host(host), m_bar(new ChooserBar(request, host.window))
{
    connect(m_bar, &ChooserBar::accepted, this, &ChooserMode::accept);
    connect(m_bar, &ChooserBar::rejected, this, &ChooserMode::cancel);
    connect(m_bar, &ChooserBar::filterChanged, this, &ChooserMode::filterChanged);

    // The window of the app that asked (X11 "x11:<hex id>"; Wayland handles
    // would need xdg-foreign).
    if (request.parentWindow.startsWith(QLatin1String("x11:"))) {
        bool ok = false;
        m_parentWindowId = request.parentWindow.mid(4).toULongLong(&ok, 16);
        if (!ok)
            m_parentWindowId = 0;
    }
    if (request.mode == Mode::Save)
        QTimer::singleShot(0, m_bar, &ChooserBar::focusName);
}

QString ChooserMode::shortTitle() const
{
    if (m_request.mode != Mode::Open)
        return tr("Save");
    return m_request.directory ? tr("Select Folder") : tr("Open");
}

QString ChooserMode::windowTitle() const
{
    return m_request.title.isEmpty() ? shortTitle() : m_request.title;
}

QString ChooserMode::fileToShow() const
{
    return m_request.mode == Mode::Save && QFileInfo(m_request.currentFile).isFile() ? m_request.currentFile : QString();
}

QString ChooserMode::startFolder() const
{
    QString start = m_request.currentFolder;
    if (start.isEmpty() || !QFileInfo(start).isDir())
        start = QSettings().value(lastFolderKey()).toString();
    if (start.isEmpty() || !QFileInfo(start).isDir())
        start = QDir::homePath();
    return start;
}

void ChooserMode::placeWindow()
{
    QWidget *window = m_host.window;
    // The window was built like a file manager window: if that one was last
    // closed maximised, this one would open maximised too.
    window->setWindowState(window->windowState() & ~(Qt::WindowMaximized | Qt::WindowFullScreen));
    const QSize size = QSettings().value("chooser/size").toSize();
    window->resize(size.isValid() ? size : QSize(960, 620));
    if (QScreen *screen = QGuiApplication::screenAt(QCursor::pos())) {
        const QRect area = screen->availableGeometry();
        window->resize(window->size().boundedTo(area.size() * 0.9)); // never larger than the screen
        window->move(area.center() - window->rect().center());
    }
}

// Written straight onto the X11 window: Qt's xcb plugin sets
// WM_TRANSIENT_FOR only for dialog-type windows (and deletes it on show() for
// others), and a dialog-type window can't be minimised or maximised under
// GNOME. Not modal either: GNOME glues modal dialogs to their parent, and
// then they can't be moved.
void ChooserMode::attachToParent()
{
#if defined(ARGONAUT_XCB)
    QWidget *window = m_host.window;
    if (!m_parentWindowId || !window->windowHandle())
        return;
    auto *x11 = qGuiApp->nativeInterface<QNativeInterface::QX11Application>();
    if (!x11 || !x11->connection())
        return; // Wayland
    const xcb_window_t parent = xcb_window_t(m_parentWindowId);
    xcb_change_property(x11->connection(), XCB_PROP_MODE_REPLACE, xcb_window_t(window->winId()),
                        XCB_ATOM_WM_TRANSIENT_FOR, XCB_ATOM_WINDOW, 32, 1, &parent);
    xcb_flush(x11->connection());
#endif
}

void ChooserMode::setUnavailableActions(const QList<QAction *> &actions)
{
    m_unavailable = actions;
    m_unavailable.removeAll(nullptr);
    keepActionsOff();
}

void ChooserMode::keepActionsOff()
{
    for (QAction *action : std::as_const(m_unavailable)) {
        action->setEnabled(false);
        action->setVisible(false);
    }
}

std::optional<ChooserRequest::Filter> ChooserMode::currentFilter() const
{
    const int index = m_bar->currentFilter();
    if (index >= 0 && index < m_request.filters.size())
        return m_request.filters.at(index);
    return std::nullopt;
}

void ChooserMode::selectionChanged()
{
    const QStringList selected = m_host.selectedPaths();
    if (m_request.mode == Mode::Save && selected.size() == 1 && !QFileInfo(selected.first()).isDir())
        m_bar->setName(QFileInfo(selected.first()).fileName()); // save over that one
    bool canAccept = true;
    if (m_request.mode == Mode::Open && !m_request.directory)
        canAccept = !selected.isEmpty();
    m_bar->setAcceptEnabled(canAccept);
}

void ChooserMode::fileActivated(const QString &path)
{
    if (m_request.mode == Mode::Save)
        m_bar->setName(QFileInfo(path).fileName());
    if (m_request.mode != Mode::SaveFiles && !m_request.directory)
        accept();
}

void ChooserMode::openSelection()
{
    const QStringList paths = m_host.selectedPaths();
    if (paths.size() == 1 && QFileInfo(paths.first()).isDir() && !m_request.directory)
        m_host.navigateTo(paths.first());
    else
        accept();
}

void ChooserMode::cancel()
{
    finish(FileChooserPortal::Cancelled);
}

void ChooserMode::accept()
{
    if (m_done)
        return;
    const QStringList selected = m_host.selectedPaths();
    const QString current = m_host.currentFolder();
    const bool realFolder = !DirectoryModel::isVirtualPath(current) && !m_host.inTrash() && QFileInfo(current).isDir();

    if (m_request.mode == Mode::Open) {
        if (m_host.inTrash() && !selected.isEmpty()) {
            m_host.showMessage(tr("Restore it from the Trash first"));
            return;
        }
        if (m_request.directory) {
            QStringList folders;
            for (const QString &path : selected)
                if (QFileInfo(path).isDir())
                    folders.append(path);
            if (folders.isEmpty()) {
                if (!realFolder) {
                    m_host.showMessage(tr("Choose a folder"));
                    return;
                }
                folders.append(current);
            }
            if (!m_request.multiple)
                folders = {folders.first()};
            finish(FileChooserPortal::Success, folders);
            return;
        }
        QStringList files;
        for (const QString &path : selected)
            if (!QFileInfo(path).isDir())
                files.append(path);
        if (files.isEmpty()) {
            if (selected.size() == 1)
                m_host.navigateTo(selected.first()); // a folder: go in
            else
                m_host.showMessage(tr("Choose a file"));
            return;
        }
        finish(FileChooserPortal::Success, files);
        return;
    }

    // Saving: into a real folder you can write to.
    QString folder = current;
    if (m_request.mode == Mode::SaveFiles && selected.size() == 1 && QFileInfo(selected.first()).isDir())
        folder = selected.first();
    else if (!realFolder) {
        m_host.showMessage(tr("Choose a folder to save in"));
        return;
    }
    if (!QFileInfo(folder).isWritable()) {
        m_host.showMessage(tr("You can't save in this folder"));
        return;
    }
    if (m_asking)
        return; // Save pressed again while the question is open

    if (m_request.mode == Mode::Save) {
        const QString name = m_bar->name().trimmed();
        if (name.isEmpty() || name == QLatin1String(".") || name == QLatin1String("..")) {
            m_bar->focusName();
            return;
        }
        QString path = name;
        if (path.startsWith(QLatin1String("~/")))
            path = QDir::homePath() + path.mid(1);
        if (!QDir::isAbsolutePath(path))
            path = QDir(folder).filePath(path);
        path = QDir::cleanPath(path);
        if (QFileInfo(path).isDir()) { // typed a folder's name: go there
            m_host.navigateTo(path);
            m_bar->setName(QString());
            return;
        }
        const QFileInfo target(path);
        if (!QFileInfo(target.absolutePath()).isDir()) {
            m_host.showMessage(tr("The folder “%1” doesn't exist").arg(target.absolutePath()));
            return;
        }
        if (target.exists() && !confirmReplace({path}))
            return;
        finish(FileChooserPortal::Success, {path});
        return;
    }
    const QStringList paths = m_request.saveFilesPaths(folder);
    QStringList existing;
    for (const QString &path : paths)
        if (QFileInfo::exists(path))
            existing.append(path);
    if (!existing.isEmpty() && !confirmReplace(existing))
        return;
    finish(FileChooserPortal::Success, paths);
}

// The app will overwrite these files, so ask first. The question runs its own
// event loop: meanwhile the app may give up (Request.Close), which closes and
// deletes the window and this object with it. So one question at a time, and
// nothing goes on if we are gone.
bool ChooserMode::confirmReplace(const QStringList &existing)
{
    const QFileInfo first(existing.first());
    const QString title = existing.size() == 1 ? tr("Replace “%1”?").arg(first.fileName())
                                               : tr("Replace %n files?", nullptr, int(existing.size()));
    const QString text = existing.size() == 1
                             ? tr("A file with this name already exists in “%1”. Saving replaces its contents.")
                                   .arg(first.dir().dirName())
                             : tr("Files with these names already exist in “%1”. Saving replaces their contents.")
                                   .arg(first.dir().dirName());
    QPointer<ChooserMode> self(this);
    QPointer<QMessageBox> box = new QMessageBox(QMessageBox::Warning, title, text, QMessageBox::NoButton, m_host.window);
    QPushButton *cancel = box->addButton(tr("Cancel"), QMessageBox::RejectRole);
    QPushButton *replace = box->addButton(tr("Replace"), QMessageBox::AcceptRole);
    box->setDefaultButton(cancel);
    m_asking = true;
    box->exec();
    if (!self) // deleted with the window
        return false;
    m_asking = false;
    const bool yes = box && box->clickedButton() == replace;
    delete box;
    return yes && !m_done;
}

void ChooserMode::finish(uint response, const QStringList &paths)
{
    if (m_done)
        return;
    m_done = true;
    QVariantMap results;
    if (response == FileChooserPortal::Success) {
        results = m_request.results(paths, m_bar->choiceValues(), m_bar->currentFilter());
        const QString current = m_host.currentFolder();
        if (!DirectoryModel::isVirtualPath(current) && !m_host.inTrash())
            QSettings().setValue(lastFolderKey(), current);
    }
    // Only the size is remembered, never the place on screen.
    if (!m_host.window->isMaximized())
        QSettings().setValue("chooser/size", m_host.window->size());
    emit finished(response, results);
}

QString ChooserMode::lastFolderKey() const
{
    QString app = m_request.appId;
    app.replace(QLatin1Char('/'), QLatin1Char('_'));
    return QStringLiteral("chooser/lastFolder/") + (app.isEmpty() ? QStringLiteral("host") : app);
}
