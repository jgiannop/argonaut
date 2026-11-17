#pragma once

#include "Claude.h"

#include <QRect>
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
// A part of a picture that has something to say when the mouse rests on it
// or presses it (the ⓘ beside a service): in the picture's own pixels.
struct Tip
{
    QRect area;
    QString text; // plain text; line breaks are kept
};

struct PreviewReply
{
    // None: "not from me", the built-in preview is used. Ansi: text with
    // colour codes (ESC [ … m), for the terminal interface, which shows the
    // colours; the window shows the text without them.
    enum Kind { None, Text, Markdown, Html, Diff, Image, Ansi } kind = None;
    QString text;          // Text, Markdown, Html (Qt's rich-text subset, no scripts, nothing loaded), Diff (a unified diff, coloured by Argonaut)
    bool monospace = false; // Text: in a fixed-width font
    QString image;         // Image: a picture file the extension wrote
    bool scroll = false;   // Image: shown at its own size and scrolled where it is taller than the pane (a long list), not scaled to fit
    QList<Tip> tips;       // Image shown at its own size (scroll): parts of it with a tooltip
    QString note;          // one line under the preview ("3 sheets")
    // A result ("show") can have a name, so the extension can replace it
    // while it is shown (a dashboard that updates) and is told when it is
    // no longer shown.
    QString extension;     // show: the extension's id
    QString resultId;      // show: its "id"; empty: a result shown once
    bool update = false;   // show: only replaces that result while it is shown; never opens the pane
    bool searchable = false; // show: a named result the user can search ("search": a field over it; what is typed goes back with searchResult)
    QString searchHint;      // show: what the empty field says ("Search services")
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
    // In the terminal interface there are no pixels: the room is in
    // character cells, and a picture would be a blur. An extension that
    // wants to look right there answers with text ("ansi").
    int columns = 0;
    int rows = 0;
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

// A row an extension adds to the sidebar (a connected server, a cloud
// drive). With a `folder` a click opens that folder; without one the
// extension is told and does what it takes (connects, then asks for the
// folder to be opened). It has an address of its own, placePath().
struct Place
{
    QString extension; // its extension's id
    QString id;
    QString label;
    QString tooltip;
    QString icon;      // "network", "cloud", "drive", "removable" or "folder"
    QString folder;    // where a click goes; empty: the extension is asked
    bool eject = false; // has a ⏏ button (disconnect); the extension is told when it is pressed
};

// Something an extension needs the user to fill in or decide while it works
// (a login, an unknown host key, which shared folder): shown as a dialog
// that says which extension is asking.
struct FormField
{
    enum Kind { Text, Secret, Check, Choice } kind = Text;
    QString id;
    QString label;
    QString value;       // what it starts with ("true" for a ticked Check)
    QString placeholder; // Text: shown while empty
    QStringList choices; // Choice: what to pick from; Text: suggestions to pick or type over
    bool alone = false;  // Check: while ticked, the other fields are off ("Connect anonymously")
};

struct Form
{
    QString extension;     // its extension's id
    QString extensionName;
    QString id;
    QString title;         // the dialog's title
    QString heading;
    QString text;
    QString warning;       // shown as a problem ("That password didn't work")
    QList<FormField> fields;
    QStringList buttons;   // besides Cancel; none given: "OK"
};

// Changes an extension proposes for a folder: items of the folder moved
// into folders inside it, and renamed. The extension only proposes: Argonaut
// checks every move (`problem` says why one can't be done), shows them for
// the user to tick, and does what is kept itself, through its own file
// engine, so it can be undone.
struct PlanMove
{
    QString item;    // a name in the folder
    QString folder;  // where it goes, relative to the folder ("Invoices/2024"); empty: it stays
    QString newName; // empty: it keeps its name
    QString reason;  // one line for the user
    QString problem; // why it can't be done; empty when it can (set by Host::checkPlan)
};

struct Plan
{
    QString extension;     // its extension's id
    QString extensionName;
    QString id;
    QString title;         // "Organize Downloads"
    QString folder;        // the folder it is about
    QString report;        // Markdown: what the extension found, shown above the moves
    QString note;          // one line under them ("Cost: $0.03")
    QList<PlanMove> moves;
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
    QStringList addresses; // the kinds of address it opens ("sftp", "smb"): typed in the path bar, they go to it
    // "uses": ["claude"]: it asks Claude things through Argonaut (with the
    // user's key, at the user's cost: the Extensions window says so).
    bool usesClaude = false;
    // An extension that is only needed now and then says so ("start":
    // "when-needed") and names what it offers here, so that can be shown
    // without its program running. The program starts when one of them is
    // used, and is stopped again after a while with nothing to do.
    bool whenNeeded = false;
    QList<Command> commands;  // "commands": as in a "commands" message
    QStringList previewTypes; // "previews": the types it previews
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
    // Which of Argonaut's interfaces this is: "window" (the default) or
    // "terminal". Extensions are told in "hello", so one that draws can
    // draw for it. Set before the first folder is shown.
    void setInterface(const QString &name) { m_interface = name; }
    QString interfaceName() const { return m_interface; }
    // A named result of `extension` is on screen now (with room for
    // `pixels`, in a window that looks like `look`), or no longer is. The
    // extension is told: {"type":"result","id":…,"shown":…,"width":…,
    // "height":…,"dark":…,"colors":{…},"font":…}.
    void resultShown(const QString &extension, const QString &id, bool shown, const QSize &pixels = QSize(),
                     const ResultLook &look = ResultLook());
    // A plan an extension sent ({"type":"plan",…}): like a form, exactly one
    // window shows it (takePlan), and it is answered once: with how many
    // moves were done and how many no longer could be, or as dismissed.
    bool takePlan(const QString &extension, const QString &id);
    void answerPlan(const QString &extension, const QString &id, bool dismissed, int applied = 0, int skipped = 0);
    // Checks moves against the folder as it is now: each gets a problem or
    // none. Also called again when the user presses Apply.
    static void checkPlan(const QString &folder, QList<PlanMove> *moves);
    static constexpr int MaxPlanMoves = 500;
    static constexpr int MaxTips = 400;     // parts of a picture with a tooltip
    static constexpr int MaxTipText = 600;  // characters of one

    // The user doesn't want to wait for Claude's answer any longer: the
    // extension is told its questions failed, and the answers are dropped
    // when they come. (What was already asked is still paid for.)
    void stopWaitingForClaude(const QString &extension);

    // What Argonaut has paid for an extension's questions to Claude, in
    // dollars (kept in the settings; the Extensions window shows it).
    static double claudeSpent(const QString &extension);

    // What the user typed in the search field of a named result that is
    // on screen and said it can be searched: {"type":"search","id":…,
    // "text":…}. The extension answers by making the result again.
    void searchResult(const QString &extension, const QString &id, const QString &text);

    // Sidebar rows from extensions, in the extensions' order.
    QList<Place> places() const;
    // "extension-place:///<extension>/<id>": the address of a row that has
    // no folder of its own.
    static QString placePath(const QString &extension, const QString &id);
    static bool isPlacePath(const QString &path);
    // The user clicked the row ("open") or its ⏏ ("eject"): its extension
    // is told {"type":"place","id":…,"action":…}.
    void activatePlace(const QString &placePath, const QString &action);

    // Addresses that aren't folders ("sftp://host/", "gdrive://…"): true if
    // a switched-on extension said in its extension.json that this kind is
    // its. It is told {"type":"address","address":…} (started first if it
    // isn't running), and answers with "open" when there is a folder.
    bool openAddress(const QString &address);
    bool handlesAddress(const QString &address) const;

    // Forms: formRequested says an extension asks. Exactly one window shows
    // it: the one whose takeForm() is true. answerForm() tells the extension
    // what was filled in (`values` by field id; a Check is "true"/"false")
    // and which button was pressed, or that the user backed out.
    bool takeForm(const QString &extension, const QString &id);
    void answerForm(const QString &extension, const QString &id, bool cancelled,
                    const QHash<QString, QString> &values = {}, int button = 0);

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
    void placesChanged();                                  // the sidebar rows from extensions
    void formRequested(const Extensions::Form &form);      // an extension asks the user something
    void planRequested(const Extensions::Plan &plan);      // an extension proposes changes: shown to be ticked and applied
    // Claude is being asked something for an extension (`waiting` questions
    // without an answer yet; 0: none any more). A window says so meanwhile.
    void claudeWaiting(const QString &extension, const QString &name, int waiting);
    // An extension asks for a folder to be shown, as the answer to something
    // the user just chose from it (a command, a sidebar row).
    void openRequested(const QString &extension, const QString &folder);
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
        QList<Place> places;              // its sidebar rows
        qint64 userActed = 0;             // when the user last chose something of its (ms since the epoch)
        bool commandsSaid = false;        // it has sent "commands": those count, not the manifest's
        bool previewsSaid = false;        // …and "previews"
        qint64 lastActive = 0;            // a when-needed one: when it last had something to do
        QSet<QString> resultsShown;       // its named results on screen: it isn't idle
        QStringList claudeWaiting;        // the questions to Claude it waits for an answer to (their ids)
        int claudePending = 0;            // requests to Claude that Argonaut is making for it
        int claudeAsked = 0;              // …and how many since the user last chose something of its
        qint64 claudeFor = 0;             // (the `userActed` that count is for)
        QString planPending;              // the plan it waits on (its id); one at a time
        bool planTaken = false;           // a window is showing it
        QString formPending;              // the form it waits on (its id); one at a time
        bool formTaken = false;           // a window is showing it
    };

    void startAsNeeded();
    void start(Running *ext);
    void stop(Running *ext);
    void send(Running *ext, const QJsonObject &message);
    void askClaude(Running *ext, const QJsonObject &message); // its "claude" message: Argonaut makes the call
    static bool isActingForUser(const Running *ext);          // the user chose something of its, not long ago
    Claude::Client *m_claude = nullptr;
    void readFrom(Running *ext);
    void handle(Running *ext, const QByteArray &line);
    // handle() tells what kind of message it is; the long ones are read here.
    void hearPlaces(Running *ext, const QJsonObject &message);
    void hearForm(Running *ext, const QJsonObject &message);
    void hearListing(Running *ext, const QJsonObject &message);
    void hearShow(Running *ext, const QJsonObject &message);
    void hearPlan(Running *ext, const QJsonObject &message);
    void hearFolder(Running *ext, const QJsonObject &message);
    QList<Command> readCommands(const Running *ext, const QJsonValue &value, int max) const;
    static PreviewReply readContent(const QJsonObject &message);
    void exited(Running *ext);
    void forget(Running *ext); // drops what it said and tells the views
    // What it offers now: what its program said, or (not said yet, or a
    // when-needed one that isn't running) what its extension.json says.
    QList<Command> commandsOf(const Running *ext) const;
    QStringList previewTypesOf(const Running *ext) const;
    bool isAvailable(const Running *ext) const; // running, or startable when needed
    void wake(Running *ext);                    // starts a when-needed one; it has something to do
    void stopIdle();

    QList<Manifest> m_manifests;
    QList<Running *> m_running; // the switched-on, usable ones, in order
    QHash<QString, int> m_watched;
    bool m_quitting = false;
    int m_previewRequest = 0;
    int m_idleMs = 60000; // ARGONAUT_EXTENSION_IDLE_MS in tests
    QString m_interface = QStringLiteral("window");
};

} // namespace Extensions

Q_DECLARE_METATYPE(Extensions::Badge)
Q_DECLARE_METATYPE(Extensions::PreviewReply)
Q_DECLARE_METATYPE(Extensions::Form)
Q_DECLARE_METATYPE(Extensions::Plan)
