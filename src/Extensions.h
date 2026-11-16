#pragma once

#include <QHash>
#include <QList>
#include <QMetaType>
#include <QObject>
#include <QSet>
#include <QSize>
#include <QString>
#include <QStringList>

class QJsonObject;
class QJsonValue;
class QProcess;

// Extensions: separate programs that add to what the file views show (core
// code, no widgets). docs/extensions.md is the guide for people writing one.
//
// An extension is a folder with an "extension.json" and a program. Argonaut
// starts the program once and talks to it in JSON, one message per line, on
// its standard input and output. It tells the extension which folders are
// shown; the extension answers with marks for the items in them (a letter or
// a dot, in one of a few colours) and a short text for the path bar. It can
// also offer commands for the right-click menu: Argonaut tells it which one
// was chosen and for which items, and the extension does the work itself.
// Extensions never get to use Argonaut's own copying, moving or undo.
//
// An extension is a program with the user's rights: one from the user's own
// folder runs only after it is switched on in Preferences. What it sends is
// data: it is checked and cut to size, and can't change what Argonaut does.
// A crash, hang or nonsense from an extension never stops Argonaut.
namespace Extensions {

constexpr int ProtocolVersion = 1;

// A mark on one item.
struct Badge
{
    bool valid = false;
    QString text;    // at most 2 characters; empty draws a dot
    QString color;   // "warning", "success", "error", "accent" or "dim"
    QString tooltip;
    bool deep = false; // also for everything inside this folder
};

// The text in the path bar for a folder.
struct Info
{
    QString text;    // empty: nothing shown
    QString tooltip;
    QString icon;    // "branch" or empty
};

// A command an extension offers in the right-click menu.
struct Command
{
    QString extension;  // its extension's id
    QString id;
    QString label;
    QString confirm;    // asked before it runs ("Discard the changes?"); empty: not asked
    QString group;      // the submenu it is in ("Git", "Git/Switch Branch"); empty: the menu itself
    bool checked = false;       // shown ticked (the current one of several)
    bool asks = false;          // wants a text from the user first (a commit message, a name)
    QString askLabel;           // what the text is
    QString askText;            // what the field starts with
    bool askMultiline = false;  // several lines
    bool onFolder = false;      // for the open folder (a right-click on empty space), not for items
    QString menu;               // "file", "edit", "view" or "go": in that menu of the menu bar (and so in the
                                // command palette) instead of the right-click menu; it is about no item
    enum Kind { Any, Files, Folders } kind = Any;
    bool multiple = true;       // false: exactly one item
    QStringList types;          // each item's type must match one ("image/*", "text/plain"); empty: any
    bool limited = false;       // only for the items in `items`
    QSet<QString> items;        // paths relative to the folder
};

// What an extension shows for a file in the preview pane (Space). Always
// text or a picture for Argonaut to show; an extension never draws itself.
struct PreviewReply
{
    enum Kind { None, Text, Markdown, Html, Diff, Image } kind = None; // None: "not from me", the built-in preview is used
    QString text;          // Text, Markdown, Html (Qt's rich-text subset, no scripts, nothing loaded), Diff (a unified diff, coloured by Argonaut)
    bool monospace = false; // Text: in a fixed-width font
    QString image;         // Image: a picture file the extension wrote
    QString note;          // one line under the preview ("3 sheets")
    // A result ("show") can have a name, so the extension can replace it
    // while it is shown (a dashboard that updates) and is told when it is
    // no longer shown.
    QString extension;     // show: the extension's id
    QString resultId;      // show: its "id"; empty: a result shown once
    bool update = false;   // show: only replaces that result while it is shown; never opens the pane
};

// How the window looks where a named result is shown, so a picture an
// extension draws can match: colours as "#rrggbb", none of them translucent.
struct ResultLook
{
    bool dark = false;
    QString window;  // the background the result is shown on
    QString surface; // a panel on it (the sidebar's colour)
    QString text;
    QString dim;     // secondary text
    QString accent;
    QString border;
    QString font;    // the family the window's text is in
};

// A list of files an extension keeps (Git: the changed files), shown in the
// file pane like a folder. It has a place's address, listingPath().
struct Listing
{
    bool valid = false;
    QString extension; // its extension's id
    QString id;
    QString title;     // "Changes on main"
    QString folder;    // the folder the files belong to (marks and commands are asked for it); may be empty
    QStringList files; // full paths
    bool previews = false; // the extension is asked for the preview of every file in it
};

// A selected item, as far as commands care.
struct Item
{
    QString path;
    QString mimeType; // what it really is
    bool isDir = false;
};

struct Manifest
{
    QString id;          // letters, digits, '-', '.', '_'
    QString name;
    QString description;
    QString version;
    QString author;
    QString folder;      // where extension.json is
    QString program;     // absolute path of the program to start
    QStringList arguments;
    bool builtIn = false; // came with Argonaut (not from the user's folder): on unless switched off
    QString problem;      // why it can't be used; empty when fine

    bool isUsable() const { return problem.isEmpty(); }
};

// Reads <folder>/extension.json. `problem` says what is wrong, if anything.
Manifest readManifest(const QString &folder, bool builtIn);

// Where the user puts extensions: $XDG_DATA_HOME/argonaut/extensions.
QString userFolder();

class Host : public QObject
{
    Q_OBJECT

public:
    static Host *instance();
    explicit Host(QObject *parent = nullptr);
    ~Host() override;

    // Every extension found, usable or not. rescan() reads the folders again.
    QList<Manifest> extensions() const;
    void rescan();

    bool isEnabled(const QString &id) const;
    void setEnabled(const QString &id, bool on);

    // A view shows `folder` / stopped showing it. Counted: every watch()
    // needs its unwatch().
    void watch(const QString &folder);
    void unwatch(const QString &folder);
    // Files in it (or in every watched folder) may have changed.
    void refresh(const QString &folder);
    void refreshAll();

    // What the extensions said. `path` is an item inside the watched `folder`.
    Badge badge(const QString &folder, const QString &path) const;
    Info info(const QString &folder) const;

    // Stops every program, for good (the application is quitting).
    void shutdown();

    // The commands for a right-click on `selection` in `folder` (empty
    // selection: on the folder's empty space), in the extensions' order.
    // `folder` may be empty (Recent, Starred): commands that aren't about a
    // folder still apply.
    QList<Command> commands(const QString &folder, const QList<Item> &selection) const;
    // The commands that asked for a place in the menu bar: `menu` is "file",
    // "edit", "view" or "go". They aren't in the right-click menu.
    QList<Command> menuCommands(const QString &menu) const;
    // Tells the extension to do it. Not for a command that asks first until
    // the user said yes. `input` is the text a command that `asks` was given.
    void invoke(const Command &command, const QString &folder, const QStringList &selection,
                const QString &input = QString());
    QString nameOf(const QString &id) const;
    // A named result of `extension` is on screen now (with room for
    // `pixels`, in a window that looks like `look`), or no longer is. The
    // extension is told: {"type":"result","id":…,"shown":…,"width":…,
    // "height":…,"dark":…,"colors":{…},"font":…}.
    void resultShown(const QString &extension, const QString &id, bool shown, const QSize &pixels = QSize(),
                     const ResultLook &look = ResultLook());

    // Previews: true if a running extension said it previews this type.
    // requestPreview() asks it and returns the request's number (0: nobody
    // to ask); the answer comes as previewReady with that number, or never
    // (the asker should not wait long, and falls back to its own preview).
    bool canPreview(const QString &mimeType) const;
    int requestPreview(const QString &path, const QString &mimeType, const QSize &pixels);
    // The same for a file shown in a listing whose extension previews its
    // files itself (Git: the file's changes), whatever the file's type.
    bool listingPreviews(const QString &listingPath) const;
    int requestListingPreview(const QString &listingPath, const QString &path, const QString &mimeType, const QSize &pixels);

    // Listings: "listing:///<extension>/<id>" is one's address in a pane.
    static QString listingPath(const QString &extension, const QString &id);
    static bool isListingPath(const QString &path);
    Listing listing(const QString &listingPath) const; // !valid: there is none (any more)

    // How an extension is doing, for the Extensions window.
    enum class State {
        Unusable, // its folder is wrong (Manifest::problem)
        Off,
        Waiting,  // on; its program starts with the first folder shown
        Running,
        Stopped,  // on, but its program kept failing and was given up
    };
    State state(const QString &id) const;

    // True while an extension's program is running (tests).
    bool isRunning(const QString &id) const;

signals:
    void folderChanged(const QString &folder); // its badges or info changed
    void extensionsChanged();                  // the list, or what is switched on
    void message(const QString &extension, const QString &text); // something to tell the user
    void previewReady(int request, const Extensions::PreviewReply &reply);
    void listingChanged(const QString &listingPath);       // its files or title changed, or it is gone
    void showListingRequested(const QString &listingPath); // the user asked to see it
    // A result to show in the preview pane (a command's answer: a diff, a report).
    void showRequested(const QString &extension, const QString &title, const Extensions::PreviewReply &content);

private:
    struct FolderData
    {
        Info info;
        QHash<QString, Badge> badges; // by path relative to the folder ("" = the folder)
        bool anyDeep = false;
        QList<Command> commands; // for this folder only
    };
    struct Running
    {
        Manifest manifest;
        QProcess *process = nullptr;
        QByteArray buffer;
        int failures = 0;
        bool stopping = false;
        QHash<QString, FolderData> folders;
        QList<Command> commands; // everywhere
        QStringList previewTypes; // what it previews ("text/markdown", "image/*")
        QHash<QString, Listing> listings; // by id
    };

    void startAsNeeded();
    void start(Running *ext);
    void stop(Running *ext);
    void send(Running *ext, const QJsonObject &message);
    void readFrom(Running *ext);
    void handle(Running *ext, const QByteArray &line);
    QList<Command> readCommands(const Running *ext, const QJsonValue &value, int max) const;
    static PreviewReply readContent(const QJsonObject &message);
    void exited(Running *ext);
    void forget(Running *ext); // drops what it said and tells the views

    QList<Manifest> m_manifests;
    QList<Running *> m_running; // the switched-on, usable ones, in order
    QHash<QString, int> m_watched;
    bool m_quitting = false;
    int m_previewRequest = 0;
};

} // namespace Extensions

Q_DECLARE_METATYPE(Extensions::Badge)
Q_DECLARE_METATYPE(Extensions::PreviewReply)
