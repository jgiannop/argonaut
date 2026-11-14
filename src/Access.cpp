#include "Access.h"

#include <QFileInfo>
#include <QMutex>
#include <QMutexLocker>

#if defined(Q_OS_UNIX)
#include <grp.h>
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>

#include <vector>
#endif

namespace {

#if defined(Q_OS_UNIX)
// The groups the current user is in, read once.
const std::vector<gid_t> &myGroups()
{
    static const std::vector<gid_t> groups = [] {
        std::vector<gid_t> list;
        const int n = ::getgroups(0, nullptr);
        if (n > 0) {
            list.resize(size_t(n));
            if (::getgroups(n, list.data()) < 0)
                list.clear();
        }
        list.push_back(::getegid());
        return list;
    }();
    return groups;
}

bool inMyGroups(uint group)
{
    for (gid_t g : myGroups()) {
        if (g == gid_t(group))
            return true;
    }
    return false;
}
#endif

QMutex namesMutex;

} // namespace

namespace Access {

bool hasOwners()
{
#if defined(Q_OS_UNIX)
    return true;
#else
    return false;
#endif
}

bool likelyWritable(QFile::Permissions permissions, uint owner, uint group)
{
#if defined(Q_OS_UNIX)
    const uid_t me = ::geteuid();
    if (me == 0)
        return true;
    if (owner == NoId)
        return permissions.testFlag(QFile::WriteOwner);
    if (uid_t(owner) == me)
        return permissions.testFlag(QFile::WriteOwner);
    if (inMyGroups(group))
        return permissions.testFlag(QFile::WriteGroup);
    return permissions.testFlag(QFile::WriteOther);
#else
    Q_UNUSED(owner);
    Q_UNUSED(group);
    return permissions.testFlag(QFile::WriteOwner); // the read-only attribute
#endif
}

QString userName(uint id)
{
    if (id == NoId)
        return {};
    QMutexLocker lock(&namesMutex);
    static QHash<uint, QString> names;
    if (auto it = names.constFind(id); it != names.constEnd())
        return *it;
    QString name = QString::number(id);
#if defined(Q_OS_UNIX)
    passwd pw{};
    passwd *result = nullptr;
    std::vector<char> buffer(16384);
    if (::getpwuid_r(uid_t(id), &pw, buffer.data(), buffer.size(), &result) == 0 && result)
        name = QString::fromLocal8Bit(pw.pw_name);
#endif
    names.insert(id, name);
    return name;
}

QString groupName(uint id)
{
    if (id == NoId)
        return {};
    QMutexLocker lock(&namesMutex);
    static QHash<uint, QString> names;
    if (auto it = names.constFind(id); it != names.constEnd())
        return *it;
    QString name = QString::number(id);
#if defined(Q_OS_UNIX)
    group gr{};
    group *result = nullptr;
    std::vector<char> buffer(16384);
    if (::getgrgid_r(gid_t(id), &gr, buffer.data(), buffer.size(), &result) == 0 && result)
        name = QString::fromLocal8Bit(gr.gr_name);
#endif
    names.insert(id, name);
    return name;
}

QString permissionString(QFile::Permissions p, bool isDir, bool isSymLink)
{
    QString s(10, QLatin1Char('-'));
    s[0] = isSymLink ? QLatin1Char('l') : isDir ? QLatin1Char('d') : QLatin1Char('-');
    const QFile::Permission bits[9] = {QFile::ReadOwner, QFile::WriteOwner, QFile::ExeOwner,
                                       QFile::ReadGroup, QFile::WriteGroup, QFile::ExeGroup,
                                       QFile::ReadOther, QFile::WriteOther, QFile::ExeOther};
    const char letters[] = "rwx";
    for (int i = 0; i < 9; ++i) {
        if (p.testFlag(bits[i]))
            s[i + 1] = QLatin1Char(letters[i % 3]);
    }
    return s;
}

bool canChangeItemsIn(const QString &dir, uint itemOwner, QHash<QString, FolderAccess> *cache)
{
    auto it = cache->constFind(dir);
    if (it == cache->constEnd()) {
        FolderAccess folder;
#if defined(Q_OS_UNIX)
        const QByteArray native = QFile::encodeName(dir);
        folder.writable = ::access(native.constData(), W_OK | X_OK) == 0;
        struct stat st;
        if (folder.writable && ::stat(native.constData(), &st) == 0) {
            folder.sticky = (st.st_mode & S_ISVTX) != 0;
            folder.owner = uint(st.st_uid);
        }
#else
        folder.writable = QFileInfo(dir).isWritable();
#endif
        it = cache->insert(dir, folder);
    }
    if (!it->writable)
        return false;
#if defined(Q_OS_UNIX)
    if (it->sticky) {
        const uint me = uint(::geteuid());
        return me == 0 || itemOwner == me || it->owner == me || itemOwner == NoId;
    }
#else
    Q_UNUSED(itemOwner);
#endif
    return true;
}

} // namespace Access
