#include "OpenWith.h"

#include "Tools.h"

#include <QFile>
#include <QLocale>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStandardPaths>

namespace {

} // namespace

namespace OpenWith {

bool isSupported()
{
#if defined(Q_OS_LINUX) || defined(Q_OS_FREEBSD)
    return !QStandardPaths::findExecutable(Tools::gio()).isEmpty();
#else
    return false;
#endif
}

Apps parseGioMime(const QString &output)
{
    Apps apps;
    QStringList *section = nullptr;
    for (const QString &raw : output.split(QLatin1Char('\n'))) {
        if (raw.startsWith(QLatin1Char('\t')) || raw.startsWith(QLatin1Char(' '))) {
            const QString id = raw.trimmed();
            if (section && !id.isEmpty() && !section->contains(id))
                section->append(id);
            continue;
        }
        const QString line = raw.trimmed();
        if (line.startsWith(QLatin1String("Default application"))) {
            apps.defaultId = line.section(QStringLiteral(": "), 1).trimmed();
            section = nullptr;
        } else if (line.startsWith(QLatin1String("Recommended applications"))) {
            section = &apps.recommendedIds;
        } else if (line.startsWith(QLatin1String("Registered applications"))) {
            section = &apps.registeredIds;
        } else {
            section = nullptr;
        }
    }
    return apps;
}

App appFromDesktopFile(const QString &desktopFile)
{
    App app;
    app.desktopFile = desktopFile;
    app.id = desktopFile.section(QLatin1Char('/'), -1);
    QFile file(desktopFile);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};

    // Name[el_GR] beats Name[el] beats Name.
    const QString locale = QLocale().name();          // "el_GR"
    const QString language = locale.section('_', 0, 0); // "el"
    QString name, nameLanguage, nameLocale;
    bool inEntry = false;
    bool hidden = false;
    while (!file.atEnd()) {
        const QString line = QString::fromUtf8(file.readLine()).trimmed();
        if (line.startsWith(QLatin1Char('['))) {
            inEntry = line == QLatin1String("[Desktop Entry]");
            continue;
        }
        if (!inEntry)
            continue;
        const int eq = int(line.indexOf(QLatin1Char('=')));
        if (eq <= 0)
            continue;
        const QString key = line.left(eq).trimmed();
        const QString value = line.mid(eq + 1).trimmed();
        if (key == QLatin1String("Name"))
            name = value;
        else if (key == QStringLiteral("Name[%1]").arg(language))
            nameLanguage = value;
        else if (key == QStringLiteral("Name[%1]").arg(locale))
            nameLocale = value;
        else if (key == QLatin1String("Icon"))
            app.icon = value;
        else if ((key == QLatin1String("NoDisplay") || key == QLatin1String("Hidden"))
                 && value == QLatin1String("true"))
            hidden = true;
    }
    app.name = !nameLocale.isEmpty() ? nameLocale : !nameLanguage.isEmpty() ? nameLanguage : name;
    if (hidden || app.name.isEmpty())
        return {};
    return app;
}

QList<App> appsFor(const QString &mimeType, QString *defaultId)
{
    QList<App> apps;
    if (!isSupported())
        return apps;
    QProcess gio;
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("LC_ALL"), QStringLiteral("C")); // parseable English output
    gio.setProcessEnvironment(env);
    gio.start(Tools::gio(), {QStringLiteral("mime"), mimeType});
    if (!gio.waitForFinished(3000))
        return apps;
    const Apps found = parseGioMime(QString::fromUtf8(gio.readAllStandardOutput()));
    if (defaultId)
        *defaultId = found.defaultId;

    QStringList order;
    if (!found.defaultId.isEmpty())
        order << found.defaultId;
    for (const QStringList *ids : {&found.recommendedIds, &found.registeredIds}) {
        for (const QString &id : *ids) {
            if (!order.contains(id))
                order << id;
        }
    }
    for (const QString &id : std::as_const(order)) {
        const QString path = QStandardPaths::locate(QStandardPaths::GenericDataLocation,
                                                    QStringLiteral("applications/") + id);
        if (path.isEmpty())
            continue;
        const App app = appFromDesktopFile(path);
        if (!app.name.isEmpty())
            apps.append(app);
    }
    return apps;
}

bool launch(const App &app, const QStringList &files)
{
    QStringList args{QStringLiteral("launch"), app.desktopFile};
    args += files;
    return QProcess::startDetached(Tools::gio(), args);
}

bool setDefault(const QString &mimeType, const App &app)
{
    if (mimeType.isEmpty() || app.id.isEmpty())
        return false;
    return QProcess::execute(Tools::gio(), {QStringLiteral("mime"), mimeType, app.id}) == 0;
}

} // namespace OpenWith
