#pragma once

#include <QHash>
#include <QList>
#include <QMetaType>
#include <QObject>
#include <QString>
#include <QStringList>

class QProcess;

// Extensions: separate programs that add to what the file views show (core
// code, no widgets). docs/extensions.md is the guide for people writing one.
//
// An extension is a folder with an "extension.json" and a program. Argonaut
// starts the program once and talks to it in JSON, one message per line, on
// its standard input and output. It tells the extension which folders are
// shown; the extension answers with marks for the items in them (a letter or
// a dot, in one of a few colours) and a short text for the path bar.
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

    // True while an extension's program is running (tests).
    bool isRunning(const QString &id) const;

signals:
    void folderChanged(const QString &folder); // its badges or info changed
    void extensionsChanged();                  // the list, or what is switched on

private:
    struct FolderData
    {
        Info info;
        QHash<QString, Badge> badges; // by path relative to the folder ("" = the folder)
        bool anyDeep = false;
    };
    struct Running
    {
        Manifest manifest;
        QProcess *process = nullptr;
        QByteArray buffer;
        int failures = 0;
        bool stopping = false;
        QHash<QString, FolderData> folders;
    };

    void startAsNeeded();
    void start(Running *ext);
    void stop(Running *ext);
    void send(Running *ext, const QJsonObject &message);
    void readFrom(Running *ext);
    void handle(Running *ext, const QByteArray &line);
    void exited(Running *ext);
    void forget(Running *ext); // drops what it said and tells the views

    QList<Manifest> m_manifests;
    QList<Running *> m_running; // the switched-on, usable ones, in order
    QHash<QString, int> m_watched;
    bool m_quitting = false;
};

} // namespace Extensions

Q_DECLARE_METATYPE(Extensions::Badge)
