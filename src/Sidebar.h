#pragma once

#include "Theme.h"

#include <QListWidget>
#include <QStringList>

class QTimer;

// The list of places at the left of the window: Recent, Starred and the
// standard folders, the Trash, bookmarks (shared with Nautilus), drives, and
// servers. It builds its own rows and keeps them current: it watches the
// bookmarks file, the mounted drives and GVfs's folder of servers, and
// rebuilds itself a moment after any of them changes.
//
// What happens when a row is clicked, right-clicked or dropped on is the
// window's business (MainWindow); this class only says what each row is.
class Sidebar : public QListWidget
{
    Q_OBJECT

public:
    enum Kind { PlaceItem, BookmarkItem, DriveItem, NetworkItem, ExtensionItem };

    explicit Sidebar(QWidget *parent = nullptr);
    ~Sidebar() override;

    // Colours for the icons and separator lines; rebuilds the rows.
    void setTheme(const Theme &theme);
    void refresh();     // rebuild the rows now
    void refreshSoon(); // …or once, a moment after a burst of changes

    // Highlights the row of `path` (the open folder), or none.
    void selectPath(const QString &path);

    // What a row is. A separator has no path.
    static QString pathOf(const QListWidgetItem *item);
    static Kind kindOf(const QListWidgetItem *item);
    static bool isSeparator(const QListWidgetItem *item);
    static bool canEject(const QListWidgetItem *item); // has an ⏏ button
    // True if `viewportPos` is on the row's ⏏ button.
    bool ejectButtonHit(const QListWidgetItem *item, const QPoint &viewportPos) const;

    // A row an extension added: its address (Extensions::Host::placePath),
    // else empty. Its path is the folder it opens, or that address when
    // the extension is to be asked.
    static QString extensionPlace(const QListWidgetItem *item);

    // The path of the "Connect to Server…" row: not a folder, a command.
    static QString connectPlace();

signals:
    void refreshed(); // the rows were rebuilt: select the open folder again

private:
    QListWidgetItem *addRow(const QString &name, const QString &path, Glyph glyph, Kind kind);
    void addSeparator();
    void watchForChanges();

    Theme m_theme;
    QTimer *m_refreshTimer;
    int m_mountsFd = -1;      // /proc/self/mounts (Linux): signals when mounts change
    QStringList m_lastMounts; // other platforms: compared every few seconds
};
