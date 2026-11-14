#include "KeyStore.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QStandardPaths>

#if defined(KOSMOS_DBUS)
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusObjectPath>
#include <QDBusVariant>
#include <QEventLoop>
#include <QTimer>
#endif

namespace {

QString keyFile()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) + QStringLiteral("/anthropic-key");
}

bool forceFile()
{
    return qEnvironmentVariable("KOSMOS_KEYSTORE") == QLatin1String("file");
}

QString readFile()
{
    QFile file(keyFile());
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return QString::fromUtf8(file.readAll()).trimmed();
}

bool writeFile(const QString &key, QString *error)
{
    QDir().mkpath(QFileInfo(keyFile()).absolutePath());
    QSaveFile file(keyFile());
    if (!file.open(QIODevice::WriteOnly)) {
        if (error)
            *error = file.errorString();
        return false;
    }
    file.setPermissions(QFile::ReadOwner | QFile::WriteOwner); // before anything is written
    file.write(key.toUtf8());
    if (!file.commit()) {
        if (error)
            *error = file.errorString();
        return false;
    }
    return true;
}

#if defined(KOSMOS_DBUS)
// ------------------------------------------------- freedesktop Secret Service

const QString kService = QStringLiteral("org.freedesktop.secrets");
const QString kPath = QStringLiteral("/org/freedesktop/secrets");
const QString kServiceIface = QStringLiteral("org.freedesktop.Secret.Service");

struct Secret
{
    QDBusObjectPath session;
    QByteArray parameters;
    QByteArray value;
    QString contentType;
};

QDBusArgument &operator<<(QDBusArgument &arg, const Secret &s)
{
    arg.beginStructure();
    arg << s.session << s.parameters << s.value << s.contentType;
    arg.endStructure();
    return arg;
}

const QDBusArgument &operator>>(const QDBusArgument &arg, Secret &s)
{
    arg.beginStructure();
    arg >> s.session >> s.parameters >> s.value >> s.contentType;
    arg.endStructure();
    return arg;
}

using Attributes = QMap<QString, QString>;
using SecretMap = QMap<QDBusObjectPath, Secret>;

} // namespace
Q_DECLARE_METATYPE(Secret)
Q_DECLARE_METATYPE(SecretMap)
namespace {

Attributes ourAttributes()
{
    return {{QStringLiteral("application"), QStringLiteral("kosmos")},
            {QStringLiteral("type"), QStringLiteral("anthropic-api-key")}};
}

QDBusConnection bus()
{
    return QDBusConnection::sessionBus();
}

bool haveKeyring()
{
    if (forceFile())
        return false;
    QDBusConnectionInterface *iface = bus().interface();
    return iface && iface->isServiceRegistered(kService).value();
}

QDBusMessage call(const QString &path, const QString &iface, const QString &method, const QVariantList &args)
{
    QDBusMessage msg = QDBusMessage::createMethodCall(kService, path, iface, method);
    msg.setArguments(args);
    return bus().call(msg, QDBus::Block, 30000);
}

// Receives a prompt's Completed(dismissed, result) signal.
class PromptWaiter : public QObject
{
    Q_OBJECT

public:
    QEventLoop loop;
    bool dismissed = true;

public slots:
    void completed(bool wasDismissed, const QDBusVariant &)
    {
        dismissed = wasDismissed;
        loop.quit();
    }
};

// Runs a prompt (unlocking the keyring asks for its password) and waits.
// False if the user dismissed it.
bool runPrompt(const QDBusObjectPath &prompt)
{
    if (prompt.path().isEmpty() || prompt.path() == QLatin1String("/"))
        return true;
    PromptWaiter waiter;
    if (!bus().connect(kService, prompt.path(), QStringLiteral("org.freedesktop.Secret.Prompt"),
                       QStringLiteral("Completed"), &waiter, SLOT(completed(bool, QDBusVariant))))
        return false;
    call(prompt.path(), QStringLiteral("org.freedesktop.Secret.Prompt"), QStringLiteral("Prompt"), {QString()});
    QTimer::singleShot(120000, &waiter.loop, &QEventLoop::quit);
    waiter.loop.exec();
    return !waiter.dismissed;
}

bool openSession(QDBusObjectPath *session)
{
    const QDBusMessage reply = call(kPath, kServiceIface, QStringLiteral("OpenSession"),
                                    {QStringLiteral("plain"), QVariant::fromValue(QDBusVariant(QString()))});
    if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().size() < 2)
        return false;
    *session = reply.arguments().at(1).value<QDBusObjectPath>();
    return true;
}

QList<QDBusObjectPath> paths(const QVariant &v)
{
    QList<QDBusObjectPath> out;
    const QDBusArgument arg = v.value<QDBusArgument>();
    arg >> out;
    return out;
}

// Unlocked items carrying our attributes (unlocking them if needed).
QList<QDBusObjectPath> findItems()
{
    const QDBusMessage reply = call(kPath, kServiceIface, QStringLiteral("SearchItems"),
                                    {QVariant::fromValue(ourAttributes())});
    if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().size() < 2)
        return {};
    QList<QDBusObjectPath> unlocked = paths(reply.arguments().at(0));
    const QList<QDBusObjectPath> locked = paths(reply.arguments().at(1));
    if (!locked.isEmpty()) {
        const QDBusMessage u = call(kPath, kServiceIface, QStringLiteral("Unlock"), {QVariant::fromValue(locked)});
        if (u.type() == QDBusMessage::ReplyMessage && u.arguments().size() >= 2) {
            unlocked += paths(u.arguments().at(0));
            if (runPrompt(u.arguments().at(1).value<QDBusObjectPath>()))
                unlocked += locked;
        }
    }
    return unlocked;
}

QString keyringLoad()
{
    const QList<QDBusObjectPath> items = findItems();
    QDBusObjectPath session;
    if (items.isEmpty() || !openSession(&session))
        return {};
    const QDBusMessage reply = call(kPath, kServiceIface, QStringLiteral("GetSecrets"),
                                    {QVariant::fromValue(QList<QDBusObjectPath>{items.first()}), QVariant::fromValue(session)});
    call(session.path(), QStringLiteral("org.freedesktop.Secret.Session"), QStringLiteral("Close"), {});
    if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().isEmpty())
        return {};
    SecretMap secrets;
    reply.arguments().at(0).value<QDBusArgument>() >> secrets;
    return secrets.isEmpty() ? QString() : QString::fromUtf8(secrets.first().value).trimmed();
}

bool keyringSave(const QString &key, QString *error)
{
    QDBusObjectPath session;
    if (!openSession(&session)) {
        if (error)
            *error = QObject::tr("The keyring didn't answer");
        return false;
    }
    const QDBusMessage alias = call(kPath, kServiceIface, QStringLiteral("ReadAlias"), {QStringLiteral("default")});
    QDBusObjectPath collection = alias.arguments().value(0).value<QDBusObjectPath>();
    if (collection.path().isEmpty() || collection.path() == QLatin1String("/"))
        collection = QDBusObjectPath(QStringLiteral("/org/freedesktop/secrets/aliases/default"));
    QVariantMap props;
    props.insert(QStringLiteral("org.freedesktop.Secret.Item.Label"), QObject::tr("Kosmos: Anthropic API key"));
    props.insert(QStringLiteral("org.freedesktop.Secret.Item.Attributes"), QVariant::fromValue(ourAttributes()));
    const Secret secret{session, QByteArray(), key.toUtf8(), QStringLiteral("text/plain")};
    QDBusMessage reply = call(collection.path(), QStringLiteral("org.freedesktop.Secret.Collection"),
                              QStringLiteral("CreateItem"), {props, QVariant::fromValue(secret), true});
    if (reply.type() == QDBusMessage::ErrorMessage && reply.errorName().contains(QLatin1String("IsLocked"))) {
        // Locked collection: unlock it (the keyring asks for its password), then again.
        const QDBusMessage u = call(kPath, kServiceIface, QStringLiteral("Unlock"),
                                    {QVariant::fromValue(QList<QDBusObjectPath>{collection})});
        if (u.type() == QDBusMessage::ReplyMessage && u.arguments().size() >= 2)
            runPrompt(u.arguments().at(1).value<QDBusObjectPath>());
        reply = call(collection.path(), QStringLiteral("org.freedesktop.Secret.Collection"),
                     QStringLiteral("CreateItem"), {props, QVariant::fromValue(secret), true});
    }
    call(session.path(), QStringLiteral("org.freedesktop.Secret.Session"), QStringLiteral("Close"), {});
    if (reply.type() != QDBusMessage::ReplyMessage) {
        if (error)
            *error = reply.errorMessage();
        return false;
    }
    if (reply.arguments().size() >= 2)
        runPrompt(reply.arguments().at(1).value<QDBusObjectPath>());
    return true;
}

bool keyringRemove()
{
    bool ok = true;
    for (const QDBusObjectPath &item : findItems()) {
        const QDBusMessage reply = call(item.path(), QStringLiteral("org.freedesktop.Secret.Item"), QStringLiteral("Delete"), {});
        ok = ok && reply.type() == QDBusMessage::ReplyMessage;
        if (reply.type() == QDBusMessage::ReplyMessage && !reply.arguments().isEmpty())
            runPrompt(reply.arguments().at(0).value<QDBusObjectPath>());
    }
    return ok;
}

void registerTypes()
{
    static bool done = false;
    if (done)
        return;
    done = true;
    qDBusRegisterMetaType<Secret>();
    qDBusRegisterMetaType<SecretMap>();
    qDBusRegisterMetaType<Attributes>();
    qDBusRegisterMetaType<QList<QDBusObjectPath>>();
}
#endif

} // namespace

namespace KeyStore {

QString load(Where *where)
{
    const QString env = qEnvironmentVariable("ANTHROPIC_API_KEY").trimmed();
    if (!env.isEmpty()) {
        if (where)
            *where = Where::Environment;
        return env;
    }
#if defined(KOSMOS_DBUS)
    registerTypes();
    if (haveKeyring()) {
        const QString key = keyringLoad();
        if (where)
            *where = key.isEmpty() ? Where::None : Where::Keyring;
        if (!key.isEmpty())
            return key;
    }
#endif
    const QString key = readFile();
    if (where)
        *where = key.isEmpty() ? Where::None : Where::File;
    return key;
}

bool save(const QString &key, Where *where, QString *error)
{
#if defined(KOSMOS_DBUS)
    registerTypes();
    if (haveKeyring()) {
        if (keyringSave(key, error)) {
            QFile::remove(keyFile()); // never in two places
            if (where)
                *where = Where::Keyring;
            return true;
        }
        return false; // don't fall back silently to a file when the keyring is there
    }
#endif
    if (!writeFile(key, error))
        return false;
    if (where)
        *where = Where::File;
    return true;
}

bool remove(QString *error)
{
    bool ok = true;
#if defined(KOSMOS_DBUS)
    registerTypes();
    if (haveKeyring())
        ok = keyringRemove();
#endif
    if (QFile::exists(keyFile()) && !QFile::remove(keyFile()))
        ok = false;
    if (!ok && error)
        *error = QObject::tr("Couldn't remove the key");
    return ok;
}

QString describe(Where where)
{
    switch (where) {
    case Where::Environment:
        return QObject::tr("the ANTHROPIC_API_KEY variable");
    case Where::Keyring:
        return QObject::tr("your keyring");
    case Where::File:
        return QObject::tr("a private file (no keyring found)");
    case Where::None:
        break;
    }
    return {};
}

} // namespace KeyStore

#if defined(KOSMOS_DBUS)
#include "KeyStore.moc"
#endif
