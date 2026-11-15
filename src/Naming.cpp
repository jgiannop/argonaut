#include "Naming.h"

#include <QCollator>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QLocale>
#include <QRegularExpression>
#include <QSet>
#include <QStandardPaths>
#include <QTextStream>

#include <algorithm>

namespace {

bool exists(const QString &path)
{
    const QFileInfo info(path);
    return info.exists() || info.isSymLink(); // a broken link still takes the name
}

// "a.txt" -> "A.txt" on a case-insensitive drive: the "existing" file is
// the item itself, which is fine.
bool sameName(const QString &a, const QString &b)
{
    return a.compare(b, Qt::CaseInsensitive) == 0;
}

QString parentOf(const QString &path)
{
    return QFileInfo(path).absolutePath();
}

} // namespace

namespace Naming {

QCollator naturalCollator()
{
    QLocale locale;
    if (locale.language() == QLocale::C)
        locale = QLocale(QLocale::English);
    QCollator collator(locale);
    collator.setNumericMode(true);
    collator.setCaseSensitivity(Qt::CaseInsensitive);
    return collator;
}

bool hasHiddenPart(const QString &relativePath)
{
    const QStringList parts = relativePath.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    return std::any_of(parts.cbegin(), parts.cend(), isHiddenName);
}

QString baseName(const QString &name, bool isDir, QString *extension)
{
    if (extension)
        extension->clear();
    if (isDir)
        return name;
    const int dot = int(name.lastIndexOf('.'));
    if (dot <= 0)
        return name;
    QString base = name.left(dot);
    QString ext = name.mid(dot);
    if (base.endsWith(QLatin1String(".tar")) && base.size() > 4) {
        base.chop(4);
        ext.prepend(QLatin1String(".tar"));
    }
    if (extension)
        *extension = ext;
    return base;
}

bool matchesPattern(const QString &name, const QString &pattern)
{
    const QString trimmed = pattern.trimmed();
    if (trimmed.isEmpty())
        return false;
    static const QRegularExpression wild(QStringLiteral("[*?\\[]"));
    if (!trimmed.contains(wild))
        return name.contains(trimmed, Qt::CaseInsensitive);
    const QRegularExpression re(QRegularExpression::wildcardToRegularExpression(trimmed),
                                QRegularExpression::CaseInsensitiveOption);
    return re.isValid() && re.match(name).hasMatch();
}

QString originalNameTag()
{
    return QStringLiteral("[Original name]");
}

QStringList numberTags()
{
    return {QStringLiteral("[1]"), QStringLiteral("[01]"), QStringLiteral("[001]")};
}

QString renamed(const QString &name, bool isDir, int n, const RenameOptions &options)
{
    QString extension;
    const QString base = baseName(name, isDir, &extension);
    QString result;
    if (options.mode == RenameOptions::Mode::Replace) {
        result = base;
        if (!options.text.isEmpty())
            result.replace(options.text, options.replacement,
                           options.caseSensitive ? Qt::CaseSensitive : Qt::CaseInsensitive);
    } else {
        result = options.text;
        const int number = options.start + n;
        const QStringList tags = numberTags();
        for (int i = 0; i < tags.size(); ++i) // [1], [01], [001]: width 1, 2, 3
            result.replace(tags[i], QStringLiteral("%1").arg(number, i + 1, 10, QLatin1Char('0')));
        result.replace(originalNameTag(), base);
    }
    return result + extension;
}

QString nameProblem(const QString &name)
{
    if (name.trimmed().isEmpty())
        return QObject::tr("The name is empty");
    if (name == QLatin1String(".") || name == QLatin1String(".."))
        return QObject::tr("“%1” isn't a valid name").arg(name);
    if (name.contains('/'))
        return QObject::tr("Names can't contain “/”");
    if (name.contains(QChar(0)))
        return QObject::tr("Names can't contain a NUL character");
#if defined(Q_OS_WIN)
    static const QRegularExpression bad(QStringLiteral(R"([<>:"\\|?*])"));
    if (name.contains(bad))
        return QObject::tr("Names can't contain any of: < > : \" \\ | ? *");
#endif
    if (name.toUtf8().size() > 255)
        return QObject::tr("The name is too long");
    return {};
}

bool checkBatch(const QList<RenameItem> &items, QStringList *problems)
{
    problems->clear();
    QSet<QString> sources;
    QHash<QString, int> targets; // target path -> how many items want it
    for (const RenameItem &item : items) {
        sources.insert(item.path);
        ++targets[parentOf(item.path) + '/' + item.newName];
    }
    bool ok = true;
    for (const RenameItem &item : items) {
        const QString target = parentOf(item.path) + '/' + item.newName;
        QString problem = nameProblem(item.newName);
        if (problem.isEmpty() && targets.value(target) > 1)
            problem = QObject::tr("Another item gets this name too");
        if (problem.isEmpty() && target != item.path && !sources.contains(target) && exists(target)
            && !sameName(target, item.path))
            problem = QObject::tr("“%1” already exists").arg(item.newName);
        ok = ok && problem.isEmpty();
        problems->append(problem);
    }
    return ok;
}

QList<TransferResult> applyBatch(const QList<RenameItem> &items, QString *error)
{
    struct Pending
    {
        QString current; // where the item is now
        QString target;
    };
    QList<Pending> pending;
    for (const RenameItem &item : items) {
        const QString target = parentOf(item.path) + '/' + item.newName;
        if (target != item.path)
            pending.append({item.path, target});
    }

    QList<TransferResult> steps;
    auto rollBack = [&steps] {
        for (auto it = steps.crbegin(); it != steps.crend(); ++it)
            QDir().rename(it->target, it->source);
        steps.clear();
    };
    auto step = [&](const QString &from, const QString &to) {
        // QDir::rename replaces an existing file on Unix: check first.
        if (exists(to) && !sameName(from, to)) {
            *error = QObject::tr("“%1” already exists").arg(QFileInfo(to).fileName());
            return false;
        }
        if (!QDir().rename(from, to)) {
            *error = QObject::tr("Couldn't rename “%1”").arg(QFileInfo(from).fileName());
            return false;
        }
        steps.append({from, to});
        return true;
    };

    while (!pending.isEmpty()) {
        // An item can go once no other pending item still sits on its target.
        int ready = -1;
        for (int i = 0; i < pending.size() && ready < 0; ++i) {
            bool blocked = false;
            for (int j = 0; j < pending.size() && !blocked; ++j)
                blocked = j != i && pending[j].current == pending[i].target;
            if (!blocked)
                ready = i;
        }
        if (ready >= 0) {
            if (!step(pending[ready].current, pending[ready].target)) {
                rollBack();
                return {};
            }
            pending.removeAt(ready);
            continue;
        }
        // Only swaps and cycles are left: park one under a temporary name.
        Pending &first = pending.first();
        const QFileInfo info(first.current);
        const QString temp = uniqueTargetPath(info.absolutePath(),
                                              QStringLiteral(".argonaut-rename-") + info.fileName(),
                                              info.isDir());
        if (!step(first.current, temp)) {
            rollBack();
            return {};
        }
        first.current = temp;
    }
    return steps;
}

} // namespace Naming

namespace Templates {

QString folder()
{
    const QString home = QDir::homePath();
    QString dir;
#if defined(Q_OS_UNIX) && !defined(Q_OS_MACOS)
    // XDG_TEMPLATES_DIR="$HOME/Templates" in user-dirs.dirs.
    QFile file(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)
               + QStringLiteral("/user-dirs.dirs"));
    bool configured = false;
    if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QTextStream in(&file);
        while (!in.atEnd()) {
            const QString line = in.readLine().trimmed();
            if (!line.startsWith(QLatin1String("XDG_TEMPLATES_DIR=")))
                continue;
            QString value = line.mid(int(qstrlen("XDG_TEMPLATES_DIR="))).trimmed();
            if (value.size() >= 2 && value.startsWith('"') && value.endsWith('"'))
                value = value.mid(1, value.size() - 2);
            if (value.startsWith(QLatin1String("$HOME")))
                value = home + value.mid(5);
            if (QDir::isAbsolutePath(value)) {
                dir = QDir::cleanPath(value);
                configured = true;
            }
        }
    }
    if (!configured)
        dir = home + QStringLiteral("/Templates");
#else
    dir = home + QStringLiteral("/Templates");
#endif
    if (QDir::cleanPath(dir) == QDir::cleanPath(home) || !QFileInfo(dir).isDir())
        return {};
    return dir;
}

QList<Entry> list(const QString &folder, int depth)
{
    QList<Entry> entries;
    if (folder.isEmpty() || depth < 0)
        return entries;
    const QFileInfoList infos = QDir(folder).entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot);
    for (const QFileInfo &info : infos) {
        if (info.fileName().startsWith('.'))
            continue;
        Entry entry;
        entry.path = info.absoluteFilePath();
        if (info.isDir()) {
            entry.isFolder = true;
            entry.name = info.fileName();
            entry.children = list(entry.path, depth - 1);
            if (entry.children.isEmpty())
                continue;
        } else if (info.isFile() && info.isReadable()) {
            entry.name = Naming::baseName(info.fileName(), false);
        } else {
            continue;
        }
        entries.append(entry);
    }
    const QCollator collator = Naming::naturalCollator();
    std::sort(entries.begin(), entries.end(), [&collator](const Entry &a, const Entry &b) {
        if (a.isFolder != b.isFolder)
            return a.isFolder; // sub-menus first, like the files view
        return collator.compare(a.name, b.name) < 0;
    });
    return entries;
}

QString create(const QString &templatePath, const QString &dir, QString *error)
{
    const QFileInfo source(templatePath);
    if (!source.isFile()) {
        if (error)
            *error = QObject::tr("The template “%1” is gone").arg(source.fileName());
        return {};
    }
    const QString target = uniqueTargetPath(dir, source.fileName(), false);
    if (!QFile::copy(templatePath, target)) { // fails rather than overwrite
        if (error)
            *error = QObject::tr("Couldn't create “%1”").arg(QFileInfo(target).fileName());
        return {};
    }
    // A read-only template shouldn't give a read-only document.
    QFile::setPermissions(target, QFile::permissions(target) | QFile::WriteOwner | QFile::ReadOwner);
    return target;
}

} // namespace Templates
