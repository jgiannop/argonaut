#include "ContentSearch.h"

#include "DirectoryModel.h"

#include <QFile>
#include <QMimeDatabase>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QUrl>

#include <vector>

namespace {

QString trackerProgram()
{
    const QString overridden = qEnvironmentVariable("ARGONAUT_TRACKER_PROGRAM");
    if (!overridden.isEmpty())
        return overridden;
    static const QString found = QStandardPaths::findExecutable(QStringLiteral("tracker3"));
    return found;
}

// Runs one SPARQL query; each result row is one line "  <value>".
QStringList runQuery(const QString &sparql, const std::function<bool()> &cancelled)
{
    const QString program = trackerProgram();
    if (program.isEmpty())
        return {};
    QProcess process;
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("LC_ALL"), QStringLiteral("C.UTF-8"));
    process.setProcessEnvironment(env);
    process.setStandardInputFile(QProcess::nullDevice());
    process.start(program, {QStringLiteral("sparql"), QStringLiteral("--dbus-service"),
                            QStringLiteral("org.freedesktop.Tracker3.Miner.Files"), QStringLiteral("-q"), sparql});
    if (!process.waitForStarted(3000))
        return {};
    for (int waited = 0; process.state() != QProcess::NotRunning; waited += 100) {
        if ((cancelled && cancelled()) || waited > 20000) {
            process.kill();
            process.waitForFinished(1000);
            return {};
        }
        process.waitForFinished(100);
    }
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0)
        return {};
    QStringList rows;
    const QStringList lines = QString::fromUtf8(process.readAllStandardOutput()).split(QLatin1Char('\n'));
    for (const QString &line : lines) {
        // "  None" is how tracker3 says "no results".
        if (line.startsWith(QLatin1String("  ")) && line.trimmed() != QLatin1String("None"))
            rows << line.mid(2).trimmed();
    }
    return rows;
}

QString sparqlString(const QString &s)
{
    QString out = s;
    out.replace(QLatin1Char('\\'), QStringLiteral("\\\\")).replace(QLatin1Char('"'), QStringLiteral("\\\""));
    return QLatin1Char('"') + out + QLatin1Char('"');
}

// foldForSearch() character by character, remembering where each folded
// character came from, so a match in the folded text maps back to the
// original ("θαλασσα" found -> "Θάλασσα" shown).
QString foldWithMap(const QString &text, std::vector<qsizetype> *origin)
{
    QString out;
    out.reserve(text.size());
    origin->clear();
    origin->reserve(size_t(text.size()));
    for (qsizetype i = 0; i < text.size(); ++i) {
        const QChar c = text.at(i);
        if (c.unicode() < 0x80) {
            out.append(c.toLower());
            origin->push_back(i);
            continue;
        }
        const QString folded = DirectoryModel::foldForSearch(QString(c));
        for (const QChar f : folded) {
            out.append(f);
            origin->push_back(i);
        }
    }
    return out;
}

QString oneLine(QString s)
{
    static const QRegularExpression spaces(QStringLiteral("\\s+"));
    return s.replace(spaces, QStringLiteral(" ")).trimmed();
}

} // namespace

namespace ContentSearch {

QStringList queryWords(const QString &query)
{
    static const QRegularExpression split(QStringLiteral("[^\\p{L}\\p{N}]+"));
    QStringList words;
    for (const QString &w : query.split(split, Qt::SkipEmptyParts)) {
        if (w.size() >= 2)
            words << w;
    }
    return words;
}

QHash<QString, QString> indexMatches(const QString &root, const QStringList &words,
                                     const std::function<bool()> &cancelled)
{
    QHash<QString, QString> found;
    if (words.isEmpty() || trackerProgram().isEmpty())
        return found;
    QString prefix = QUrl::fromLocalFile(root).toString(QUrl::FullyEncoded);
    if (!prefix.endsWith(QLatin1Char('/')))
        prefix += QLatin1Char('/');
    // Tracker's full-text index can be incomplete (entries that failed to
    // index are missing from it, with no way to tell), so the stored text
    // is searched directly: complete, and fast enough for a background step.
    // One column per row: "<url>\t<text around the first word>", one line.
    const QString first = sparqlString(words.first().toLower());
    QStringList conditions;
    for (const QString &w : words)
        conditions << QStringLiteral("CONTAINS(LCASE(?t), %1)").arg(sparqlString(w.toLower()));
    const QString query =
        QStringLiteral(
            "SELECT (CONCAT(?url, \"\\t\", REPLACE(SUBSTR(?t, "
            "IF(STRLEN(STRBEFORE(LCASE(?t), %1)) > 40, STRLEN(STRBEFORE(LCASE(?t), %1)) - 39, 1), 110), "
            "\"[\\\\r\\\\n\\\\t]+\", \" \")) AS ?row) "
            "WHERE { ?d nie:plainTextContent ?t ; nie:isStoredAs ?f . ?f nie:url ?url . "
            "FILTER(STRSTARTS(?url, %2) && %3) } LIMIT 2000")
            .arg(first, sparqlString(prefix), conditions.join(QStringLiteral(" && ")));
    for (const QString &row : runQuery(query, cancelled)) {
        const qsizetype tab = row.indexOf(QLatin1Char('\t'));
        const QString url = tab < 0 ? row : row.left(tab);
        const QString path = QUrl(url).toLocalFile();
        if (path.isEmpty())
            continue;
        QString snippet = tab < 0 ? QString() : oneLine(row.mid(tab + 1));
        if (!snippet.isEmpty())
            snippet = QChar(0x2026) + snippet + QChar(0x2026);
        found.insert(path, snippet);
    }
    return found;
}

bool isTextFile(const QString &name, qint64 size)
{
    if (size <= 0 || size > MaxTextBytes)
        return false;
    static const QMimeDatabase db;
    const QMimeType mime = db.mimeTypeForFile(name, QMimeDatabase::MatchExtension);
    if (mime.inherits(QStringLiteral("text/plain")))
        return true;
    // Also: JSON, XML, SVG, scripts and other textual types.
    static const QStringList textual = {QStringLiteral("application/json"), QStringLiteral("application/xml"),
                                        QStringLiteral("application/javascript"), QStringLiteral("application/x-shellscript"),
                                        QStringLiteral("application/sql"), QStringLiteral("application/toml"),
                                        QStringLiteral("application/x-yaml"), QStringLiteral("image/svg+xml")};
    for (const QString &t : textual) {
        if (mime.inherits(t))
            return true;
    }
    return false;
}

QString snippetAround(const QString &text, qsizetype at, qsizetype length, int width)
{
    const qsizetype half = qMax<qsizetype>(10, (width - length) / 2);
    qsizetype start = qMax<qsizetype>(0, at - half);
    qsizetype end = qMin(text.size(), at + length + half);
    // Stay on the line of the match.
    const qsizetype lineStart = text.lastIndexOf(QLatin1Char('\n'), at);
    if (lineStart >= start)
        start = lineStart + 1;
    const qsizetype lineEnd = text.indexOf(QLatin1Char('\n'), at + length);
    if (lineEnd >= 0 && lineEnd < end)
        end = lineEnd;
    QString out = oneLine(text.mid(start, end - start));
    if (start > 0 && text.at(start - 1) != QLatin1Char('\n'))
        out.prepend(QChar(0x2026));
    if (end < text.size() && text.at(end) != QLatin1Char('\n'))
        out.append(QChar(0x2026));
    return out;
}

bool textMatches(const QString &path, const QStringList &foldedWords, QString *snippet)
{
    if (foldedWords.isEmpty())
        return false;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return false;
    const QByteArray bytes = file.read(MaxTextBytes);
    if (bytes.left(4096).contains('\0'))
        return false; // binary after all
    const QString text = QString::fromUtf8(bytes);
    const QString folded = DirectoryModel::foldForSearch(text);
    // Folding can change lengths (accents), so positions are looked up
    // again in the original text for the snippet.
    for (const QString &w : foldedWords) {
        if (!folded.contains(w))
            return false;
    }
    if (snippet) {
        const QString &first = foldedWords.first();
        std::vector<qsizetype> origin;
        const QString mapped = foldWithMap(text, &origin);
        const qsizetype at = mapped.indexOf(first);
        if (at >= 0 && size_t(at + first.size()) <= origin.size()) {
            const qsizetype start = origin[size_t(at)];
            const qsizetype end = origin[size_t(at + first.size() - 1)] + 1;
            *snippet = snippetAround(text, start, end - start);
        } else {
            *snippet = snippetAround(folded, folded.indexOf(first), first.size());
        }
    }
    return true;
}

} // namespace ContentSearch
