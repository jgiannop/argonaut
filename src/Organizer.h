#pragma once

#include "Claude.h"

#include <QDateTime>
#include <QJsonArray>
#include <QObject>
#include <QStringList>

// Organize a folder with Claude (core code, no widgets).
//
// Kosmos describes the folder's items (names, sizes, dates, real types;
// sub-folders by their item count and a few names inside) and Claude
// answers through two tools:
//  - read_files: asks to read the beginning of some files whose names say
//    little. Not offered in NamesOnly mode; in Ask mode the user decides
//    (readRequest / allowReading). Excerpts: text, the first pages of a
//    PDF (pdftotext in a bubblewrap sandbox), a small preview of an image.
//    Programs (Dangerous) are never read.
//  - propose_plan: a report and moves (into sub-folders, optionally renamed).
// Claude never touches files: the plan is checked (validate) and shown, and
// the window carries out what the user keeps, through FileOperations.
//
// Names and file contents are untrusted: they go to Claude inside
// <inventory> / tool results as data, and the plan can only move and rename
// items of this folder into folders inside it.
class Organizer : public QObject
{
    Q_OBJECT

public:
    struct Item
    {
        QString name;
        bool isDir = false;
        qint64 size = 0;
        QDateTime modified;
        QString type;        // "PDF document" (real type, from the contents)
        QString mime;
        bool dangerous = false;
        bool project = false; // a folder with .git, package.json, CMakeLists.txt…
        int childCount = 0;
        QStringList samples;  // a few names inside a folder
    };

    struct Move
    {
        QString item;    // name in the folder
        QString folder;  // relative target folder ("Invoices/2024"), empty to stay
        QString newName; // empty to keep the name
        QString reason;
        QString problem; // why it can't be done (set by validate), empty if fine
    };

    static constexpr int MaxItems = 500;      // per run
    static constexpr int MaxReadRounds = 3;   // read_files calls answered
    static constexpr int MaxFilesPerRead = 20;
    static constexpr int ExcerptChars = 1500;

    Organizer(const QString &folder, Claude::Client *client, QObject *parent = nullptr);

    // Reads the folder (top level, without hidden items). Fast; no network.
    void scan();
    const QList<Item> &items() const { return m_items; }
    int skipped() const { return m_skipped; } // items beyond MaxItems
    QString folder() const { return m_folder; }

    void setMode(Claude::Mode mode) { m_mode = mode; }
    Claude::Mode mode() const { return m_mode; }
    void setWishes(const QString &wishes) { m_wishes = wishes; }

    // Tokens and dollars of the first request (count_tokens: free).
    void estimate(std::function<void(qint64 tokens, double dollars, const QString &error)> done);
    void start();
    void allowReading(bool allow); // answers readRequest
    void cancel();
    double dollars() const { return m_dollars; }

    // The pieces, exposed for tests.
    QJsonObject request() const; // the request with the conversation so far
    static QString systemPrompt();
    QString inventory() const;
    static QJsonArray tools(bool canRead);
    // Checks a plan against the folder: every move gets a problem or none.
    static void validate(const QString &folder, const QList<Item> &items, QList<Move> *moves);
    // The tool result for reading these files (text, image blocks).
    static QJsonArray excerpts(const QString &folder, const QList<Item> &items, const QStringList &names);

signals:
    void progress(const QString &text);
    void readRequest(const QStringList &files, const QString &reason);
    void finished(const QString &report, const QList<Organizer::Move> &moves);
    void failed(const QString &error);

private:
    void send();
    void handle(const Claude::Response &response);
    void answerRead(bool allow);

    QString m_folder;
    Claude::Client *m_client;
    Claude::Mode m_mode = Claude::Mode::Ask;
    QString m_wishes;
    QList<Item> m_items;
    int m_skipped = 0;
    QJsonArray m_messages;      // the conversation
    QJsonObject m_pendingRead;  // the read_files tool_use waiting for the user
    int m_readRounds = 0;
    double m_dollars = 0;
    bool m_cancelled = false;
};

Q_DECLARE_METATYPE(Organizer::Move)
