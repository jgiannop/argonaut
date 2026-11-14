#pragma once

#include <QHash>
#include <QString>
#include <QStringList>

#include <functional>

// Searching inside files (core code, used by DirectoryModel's search walk).
//
// Two sources, because neither covers everything:
//  - GNOME's search index (LocalSearch / Tracker) for documents only it can
//    read (PDF, office files…), but only in the folders it indexes.
//    Its full-text index can be incomplete (Tracker 3.7 with newer SQLite
//    drops entries), so the stored text is searched instead. Case doesn't
//    matter there, but accents do.
//  - Plain-text files (code, notes, csv, json, ini…) read directly during
//    the walk, so any folder works.
// Every word must appear; case and accents don't matter.
namespace ContentSearch {

// Words safe to put in a query: letters and digits only, at least 2 long.
QStringList queryWords(const QString &query);

// Documents under `root` whose indexed text has every word: path -> a
// short snippet ("…the invoice for March…", may be empty). Blocking (run
// it on a worker); empty if there's no index. `cancelled` is polled.
// ARGONAUT_TRACKER_PROGRAM replaces `tracker3` (tests).
QHash<QString, QString> indexMatches(const QString &root, const QStringList &words,
                                     const std::function<bool()> &cancelled = {});

// True for files worth reading as text, judged by name (and a size limit).
constexpr qint64 MaxTextBytes = 4 * 1024 * 1024;
bool isTextFile(const QString &name, qint64 size);

// Reads a text file; true if it has every folded word (see
// DirectoryModel::foldForSearch). `snippet` gets the text around the first.
// Files with NUL bytes (binary) never match.
bool textMatches(const QString &path, const QStringList &foldedWords, QString *snippet);

// The text around `at` on one line, about `width` characters, with "…".
QString snippetAround(const QString &text, qsizetype at, qsizetype length, int width = 90);

} // namespace ContentSearch
