#include "RecentFiles.h"

#include <QDateTime>
#include <QDir>
#include <QDomDocument>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>
#include <QUrl>
#include <QXmlStreamReader>

#include <algorithm>

namespace {

const QString BookmarkNs = QStringLiteral("http://www.freedesktop.org/standards/desktop-bookmarks");
const QString MimeNs = QStringLiteral("http://www.freedesktop.org/standards/shared-mime-info");

qint64 parseTime(QStringView text)
{
    if (text.isEmpty())
        return 0;
    const QDateTime t = QDateTime::fromString(text.toString(), Qt::ISODateWithMs);
    return t.isValid() ? t.toMSecsSinceEpoch() : 0;
}

QString now()
{
    // GTK writes microseconds with a Z; milliseconds are fine for readers.
    return QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
}

bool loadDom(QDomDocument *doc)
{
    QFile file(RecentFiles::filePath());
    if (!file.exists()) {
        doc->setContent(QStringLiteral(
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<xbel version=\"1.0\"\n"
            "      xmlns:bookmark=\"%1\"\n      xmlns:mime=\"%2\"\n>\n</xbel>\n").arg(BookmarkNs, MimeNs));
        return true;
    }
    if (!file.open(QIODevice::ReadOnly))
        return false;
    // Not namespace-processed: element names stay as written ("bookmark:application"),
    // so what we write back looks like what GTK wrote.
    return bool(doc->setContent(&file));
}

bool saveDom(const QDomDocument &doc)
{
    const QString path = RecentFiles::filePath();
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    file.write(doc.toByteArray(2));
    if (!file.commit())
        return false;
    QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    return true;
}

QString hrefFor(const QString &path)
{
    return QString::fromUtf8(QUrl::fromLocalFile(path).toEncoded());
}

} // namespace

namespace RecentFiles {

QString filePath()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)
           + QStringLiteral("/recently-used.xbel");
}

QList<Item> read(int max)
{
    QList<Item> items;
    QFile file(filePath());
    if (!file.open(QIODevice::ReadOnly))
        return items;
    QXmlStreamReader xml(&file);
    QSet<QString> seen;
    while (!xml.atEnd()) {
        if (xml.readNext() != QXmlStreamReader::StartElement || xml.name() != QLatin1String("bookmark"))
            continue;
        const QXmlStreamAttributes a = xml.attributes();
        const QUrl url(a.value(QLatin1String("href")).toString());
        if (!url.isLocalFile())
            continue;
        const QString path = QDir::cleanPath(url.toLocalFile());
        if (seen.contains(path))
            continue;
        seen.insert(path);
        const qint64 used = std::max({parseTime(a.value(QLatin1String("added"))),
                                      parseTime(a.value(QLatin1String("modified"))),
                                      parseTime(a.value(QLatin1String("visited")))});
        items.append({path, used});
    }
    std::sort(items.begin(), items.end(), [](const Item &x, const Item &y) { return x.used > y.used; });
    if (items.size() > max)
        items.resize(max);
    return items;
}

bool rememberEnabled()
{
    const QString forced = qEnvironmentVariable("ARGONAUT_REMEMBER_RECENT");
    if (!forced.isEmpty())
        return forced != QLatin1String("0");
#if defined(Q_OS_LINUX)
    static const bool enabled = [] {
        if (QStandardPaths::findExecutable("gsettings").isEmpty())
            return true;
        QProcess p;
        p.start("gsettings", {"get", "org.gnome.desktop.privacy", "remember-recent-files"});
        if (!p.waitForFinished(2000))
            return true;
        return p.readAllStandardOutput().trimmed() != "false";
    }();
    return enabled;
#else
    return true;
#endif
}

bool add(const QString &path, const QString &mimeType)
{
    if (!rememberEnabled())
        return false;
    QDomDocument doc;
    if (!loadDom(&doc))
        return false; // unreadable or broken: never overwrite it
    QDomElement root = doc.documentElement();
    if (root.tagName() != QLatin1String("xbel"))
        return false;
    const QString href = hrefFor(QDir::cleanPath(path));
    const QString stamp = now();

    QDomElement bookmark;
    for (QDomElement b = root.firstChildElement("bookmark"); !b.isNull(); b = b.nextSiblingElement("bookmark")) {
        if (b.attribute("href") == href) {
            bookmark = b;
            break;
        }
    }
    if (bookmark.isNull()) {
        bookmark = doc.createElement("bookmark");
        bookmark.setAttribute("href", href);
        bookmark.setAttribute("added", stamp);
        QDomElement info = doc.createElement("info");
        QDomElement metadata = doc.createElement("metadata");
        metadata.setAttribute("owner", "http://freedesktop.org");
        QDomElement mime = doc.createElement("mime:mime-type");
        mime.setAttribute("type", mimeType.isEmpty() ? QStringLiteral("application/octet-stream") : mimeType);
        metadata.appendChild(mime);
        metadata.appendChild(doc.createElement("bookmark:applications"));
        info.appendChild(metadata);
        bookmark.appendChild(info);
        root.appendChild(bookmark);
    }
    bookmark.setAttribute("modified", stamp);
    bookmark.setAttribute("visited", stamp);

    // <bookmark:application name="Argonaut" … count="N"/>
    QDomElement apps = bookmark.firstChildElement("info").firstChildElement("metadata")
                           .firstChildElement("bookmark:applications");
    if (apps.isNull())
        return false; // a shape we don't know: leave it alone
    QDomElement app;
    for (QDomElement e = apps.firstChildElement("bookmark:application"); !e.isNull();
         e = e.nextSiblingElement("bookmark:application")) {
        if (e.attribute("name") == QLatin1String("Argonaut"))
            app = e;
    }
    if (app.isNull()) {
        app = doc.createElement("bookmark:application");
        app.setAttribute("name", "Argonaut");
        app.setAttribute("exec", "'argonaut %u'");
        app.setAttribute("count", "0");
        apps.appendChild(app);
    }
    app.setAttribute("modified", stamp);
    app.setAttribute("count", QString::number(app.attribute("count").toInt() + 1));
    return saveDom(doc);
}

bool remove(const QStringList &paths)
{
    QSet<QString> hrefs;
    for (const QString &p : paths)
        hrefs.insert(hrefFor(QDir::cleanPath(p)));
    QDomDocument doc;
    if (!QFile::exists(filePath()) || !loadDom(&doc))
        return false;
    QDomElement root = doc.documentElement();
    bool changed = false;
    for (QDomElement b = root.firstChildElement("bookmark"); !b.isNull();) {
        const QDomElement next = b.nextSiblingElement("bookmark");
        if (hrefs.contains(b.attribute("href"))) {
            root.removeChild(b);
            changed = true;
        }
        b = next;
    }
    return changed && saveDom(doc);
}

} // namespace RecentFiles
