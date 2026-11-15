#pragma once

#include "FileChooserRequest.h"

#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantMap>

#include <functional>
#include <optional>

class ChooserBar;
class QAction;
class QWidget;

// What makes a window an Open/Save window for another app
// (`argonaut --portal`, see FileChooserPortal): the bar at the bottom, what
// "accept" means for the request, the one answer, and where the window goes
// on screen. The window itself (MainWindow) stays a file browser: it makes
// one ChooserMode in startChooser() and tells it what the user did.
//
// It never opens, writes or deletes a file: the answer is a list of paths
// and the app that asked does the rest.
class ChooserMode : public QObject
{
    Q_OBJECT

public:
    // What the mode needs from the window it lives in.
    struct Host
    {
        QWidget *window = nullptr;
        std::function<QStringList()> selectedPaths;
        std::function<QString()> currentFolder; // may be a place ("recent:///")
        std::function<bool()> inTrash;
        std::function<bool(const QString &folder)> navigateTo;
        std::function<void(const QString &text)> showMessage;
    };

    ChooserMode(const ChooserRequest &request, const Host &host, QObject *parent = nullptr);

    const ChooserRequest &request() const { return m_request; }
    ChooserBar *bar() const { return m_bar; } // goes under the file views
    bool isDone() const { return m_done; }

    // The app's own title (it can be long), and a short one for the header.
    QString windowTitle() const;
    QString shortTitle() const;

    // Where to start: a file to show selected (Save, an existing file being
    // saved again), else a folder (the one asked for, the last one this app
    // used, or home).
    QString fileToShow() const;
    QString startFolder() const;
    // The remembered size, in the middle of the screen with the mouse (where
    // the app that asked was just clicked). Never the file manager's own
    // geometry or maximised state.
    void placeWindow();
    // Ties the window to the asking app's window (X11), so the desktop keeps
    // it above that app. Call after every show().
    void attachToParent();

    // Commands an Open/Save window doesn't offer (other windows, quitting…):
    // they are switched off now and again after every keepActionsOff().
    void setUnavailableActions(const QList<QAction *> &actions);
    void keepActionsOff();

    // The filter chosen in the bar, if any.
    std::optional<ChooserRequest::Filter> currentFilter() const;

    // What the user did in the window.
    void selectionChanged();                 // updates the name field and the accept button
    void fileActivated(const QString &path); // a file was double-clicked or opened
    void openSelection();                    // Enter: go into the one folder, else accept
    void accept();                           // the accept button
    void cancel();                           // Cancel, Esc, the window closing, or the app gave up

signals:
    // The one answer (FileChooserPortal's response and results). The window
    // should close after it.
    void finished(uint response, const QVariantMap &results);
    void filterChanged();

private:
    bool confirmReplace(const QStringList &existing);
    void finish(uint response, const QStringList &paths = {});
    QString lastFolderKey() const;

    ChooserRequest m_request;
    Host m_host;
    ChooserBar *m_bar = nullptr;
    QList<QAction *> m_unavailable;
    quint64 m_parentWindowId = 0; // the asking app's X11 window, or 0
    bool m_done = false;          // answered: nothing more is sent
    bool m_asking = false;        // the "Replace?" question is open
};
