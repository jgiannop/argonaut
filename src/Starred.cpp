#include "Starred.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUrl>

Starred &Starred::instance()
{
    static Starred *starred = new Starred; // lives until exit
    return *starred;
}

Starred::Starred()
{
    load();
    // Watch its folder if it exists, else the data folder above it (no
    // folders are created until something is starred).
    const QString dir = QFileInfo(filePath()).absolutePath();
    m_watcher.addPath(QFileInfo(dir).isDir() ? dir : QFileInfo(dir).absolutePath());
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, [this] {
        if (m_saving)
            return;
        const QStringList before = m_order;
        load();
        if (m_order != before)
            emit changed();
    });
}

QString Starred::filePath() const
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)
           + QStringLiteral("/argonaut/starred");
}

void Starred::load()
{
    m_order.clear();
    m_set.clear();
    QFile file(filePath());
    if (!file.open(QIODevice::ReadOnly))
        return;
    while (!file.atEnd()) {
        const QUrl url(QString::fromUtf8(file.readLine()).trimmed());
        if (!url.isLocalFile())
            continue;
        const QString path = QDir::cleanPath(url.toLocalFile());
        if (!m_set.contains(path)) {
            m_set.insert(path);
            m_order.append(path);
        }
    }
}

void Starred::save()
{
    QDir().mkpath(QFileInfo(filePath()).absolutePath());
    QSaveFile file(filePath());
    if (!file.open(QIODevice::WriteOnly))
        return;
    for (const QString &path : std::as_const(m_order))
        file.write(QUrl::fromLocalFile(path).toEncoded() + '\n');
    m_saving = true;
    file.commit();
    m_saving = false;
}

bool Starred::isStarred(const QString &path) const
{
    return m_set.contains(path);
}

QStringList Starred::paths() const
{
    return m_order;
}

void Starred::setStarred(const QStringList &paths, bool starred)
{
    bool changedAny = false;
    for (const QString &p : paths) {
        const QString path = QDir::cleanPath(p);
        if (starred && !m_set.contains(path)) {
            m_set.insert(path);
            m_order.prepend(path);
            changedAny = true;
        } else if (!starred && m_set.remove(path)) {
            m_order.removeAll(path);
            changedAny = true;
        }
    }
    if (changedAny) {
        save();
        emit changed();
    }
}

void Starred::renamed(const QString &from, const QString &to)
{
    const QString src = QDir::cleanPath(from);
    const QString dst = QDir::cleanPath(to);
    const QString prefix = src + QLatin1Char('/');
    bool changedAny = false;
    for (QString &path : m_order) {
        QString next;
        if (path == src)
            next = dst;
        else if (path.startsWith(prefix))
            next = dst + path.mid(src.size());
        if (next.isEmpty() || m_set.contains(next))
            continue;
        m_set.remove(path);
        m_set.insert(next);
        path = next;
        changedAny = true;
    }
    if (changedAny) {
        save();
        emit changed();
    }
}
