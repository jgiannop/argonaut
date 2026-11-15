#include "Tools.h"

#include <QHash>
#include <QMutex>
#include <QStandardPaths>

namespace Tools {

QString program(const char *variable, const QString &name)
{
    const QString overridden = qEnvironmentVariable(variable);
    return overridden.isEmpty() ? name : overridden;
}

QString find(const char *variable, const QString &name)
{
    const QString overridden = qEnvironmentVariable(variable);
    if (!overridden.isEmpty())
        return overridden;
    static QMutex mutex;
    static QHash<QString, QString> found;
    QMutexLocker lock(&mutex);
    auto it = found.constFind(name);
    if (it == found.constEnd())
        it = found.insert(name, QStandardPaths::findExecutable(name));
    return *it;
}

QString gio()
{
    return program("ARGONAUT_GIO_PROGRAM", QStringLiteral("gio"));
}

} // namespace Tools
