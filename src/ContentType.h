#pragma once

#include <QAtomicInt>
#include <QHash>
#include <QMimeType>
#include <QObject>
#include <QString>
#include <QTimer>

// Checks what a file really is, from its first bytes, and compares that with
// what its name says. Nautilus/GIO trusts any extension that only one type
// uses (".pdf", ".zip", ".jpg"), so a broken download that's really an HTML
// page, or a program named "invoice.pdf", shows as a document. We read the
// start of every file instead, in the background, remembering the answer.
namespace ContentType {

enum class Verdict : quint8 {
    NotChecked = 0,
    Consistent,  // content agrees with the name (or can't tell: empty, unknown data)
    Identified,  // the name says nothing (no/unknown extension); content tells
    Mismatch,    // the content is a different kind of file
    Dangerous,   // a program disguised as something that isn't one
};

struct Result
{
    QString nameMime;    // what the name says
    QString contentMime; // what the first bytes say
    Verdict verdict = Verdict::NotChecked;
};

constexpr int HeadSize = 4096; // bytes read per file

Verdict judge(const QMimeType &byName, const QMimeType &byContent);
bool isProgram(const QMimeType &type);
// From a file name and its first bytes (no disk access; for tests).
Result inspectData(const QString &fileName, const QByteArray &head);
// Read the first HeadSize bytes of `path` and judge them.
Result inspect(const QString &path);
// Number of files inspect() has read (tests check the cache avoids reads).
int readCount();

// The type to show and open with: the content's when it tells us more.
QString effectiveMime(const Result &result);
// Human-readable explanation for tooltips and Properties, or empty.
QString describe(const Result &result);
// "report.pdf" whose content is HTML -> "report.html"; empty if no fix.
QString fixedName(const QString &fileName, const QString &contentMime);

// Remembered results, on disk (~/.cache/…), keyed by path + size + mtime:
// each file is read once, ever, until it changes. GUI thread only.
class Cache : public QObject
{
    Q_OBJECT

public:
    static Cache &instance();
    bool lookup(const QString &path, qint64 size, qint64 mtime, Result *result);
    void store(const QString &path, qint64 size, qint64 mtime, const Result &result);
    void save();
    QString filePath() const;

private:
    Cache();
    void load();
    struct Entry
    {
        qint64 size = 0;
        qint64 mtime = 0;
        QString nameMime;
        QString contentMime;
        quint8 verdict = 0;
        quint32 stamp = 0; // for dropping the oldest when the cache is full
    };
    QHash<QString, Entry> m_entries;
    quint32 m_nextStamp = 1;
    bool m_loaded = false;
    bool m_dirty = false;
    QTimer m_saveTimer;
};

} // namespace ContentType
