#include "Remote.h"

#include "Tools.h"

#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QUrl>

namespace {

// "host=x,user=y,port=22" -> {host: x, user: y, port: 22}; values are
// %-encoded by GVfs (a share name with a comma, a WebDAV prefix with '/').
QHash<QString, QString> keys(const QString &spec)
{
    QHash<QString, QString> out;
    for (const QString &part : spec.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
        const qsizetype eq = part.indexOf(QLatin1Char('='));
        if (eq > 0)
            out.insert(part.left(eq), QUrl::fromPercentEncoding(part.mid(eq + 1).toUtf8()));
    }
    return out;
}

QString hostPart(const QHash<QString, QString> &k, const QString &hostKey)
{
    QString host = k.value(hostKey);
    if (host.contains(QLatin1Char(':')) && !host.startsWith(QLatin1Char('[')))
        host = QLatin1Char('[') + host + QLatin1Char(']'); // IPv6
    QString out;
    if (k.contains(QStringLiteral("user")))
        out += QString::fromUtf8(QUrl::toPercentEncoding(k.value(QStringLiteral("user")))) + QLatin1Char('@');
    out += host;
    if (k.contains(QStringLiteral("port")))
        out += QLatin1Char(':') + k.value(QStringLiteral("port"));
    return out;
}

} // namespace

namespace Remote {

bool isRemoteAddress(const QString &text)
{
    static const QRegularExpression re(QStringLiteral("^(sftp|ssh|smb|ftp|ftps|dav|davs|webdav|webdavs|afp|nfs|mtp)://"),
                                       QRegularExpression::CaseInsensitiveOption);
    return re.match(text.trimmed()).hasMatch();
}

QString gvfsRoot()
{
    const QString overridden = qEnvironmentVariable("ARGONAUT_GVFS_ROOT");
    if (!overridden.isEmpty())
        return QDir::cleanPath(overridden);
#if defined(Q_OS_LINUX)
    const QString runtime = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    if (!runtime.isEmpty() && QFileInfo(runtime + QStringLiteral("/gvfs")).isDir())
        return QDir::cleanPath(runtime + QStringLiteral("/gvfs"));
#endif
    return {};
}

bool parseMountDir(const QString &dirName, Mount *mount)
{
    const qsizetype colon = dirName.indexOf(QLatin1Char(':'));
    if (colon <= 0)
        return false;
    const QString type = dirName.left(colon);
    const QHash<QString, QString> k = keys(dirName.mid(colon + 1));
    Mount m;
    if (type == QLatin1String("sftp") || type == QLatin1String("ftp") || type == QLatin1String("ftps")
        || type == QLatin1String("afp-volume") || type == QLatin1String("nfs")) {
        if (!k.contains(QStringLiteral("host")))
            return false;
        m.scheme = type == QLatin1String("afp-volume") ? QStringLiteral("afp") : type;
        QString path = QStringLiteral("/");
        if (type == QLatin1String("afp-volume") && k.contains(QStringLiteral("volume")))
            path += k.value(QStringLiteral("volume")) + QLatin1Char('/');
        if (type == QLatin1String("nfs") && k.contains(QStringLiteral("prefix")))
            path = k.value(QStringLiteral("prefix")) + QLatin1Char('/');
        m.address = m.scheme + QStringLiteral("://") + hostPart(k, QStringLiteral("host")) + path;
        const QString user = k.value(QStringLiteral("user"));
        m.name = user.isEmpty() ? k.value(QStringLiteral("host")) : user + QLatin1Char('@') + k.value(QStringLiteral("host"));
    } else if (type == QLatin1String("smb-share")) {
        if (!k.contains(QStringLiteral("server")) || !k.contains(QStringLiteral("share")))
            return false;
        m.scheme = QStringLiteral("smb");
        QHash<QString, QString> kk = k;
        if (kk.contains(QStringLiteral("domain")) && kk.contains(QStringLiteral("user")))
            kk.insert(QStringLiteral("user"), kk.value(QStringLiteral("domain")) + QLatin1Char(';') + kk.value(QStringLiteral("user")));
        m.address = QStringLiteral("smb://") + hostPart(kk, QStringLiteral("server")) + QLatin1Char('/')
                    + QString::fromUtf8(QUrl::toPercentEncoding(k.value(QStringLiteral("share")))) + QLatin1Char('/');
        m.name = QObject::tr("%1 on %2").arg(k.value(QStringLiteral("share")), k.value(QStringLiteral("server")));
    } else if (type == QLatin1String("dav") || type == QLatin1String("davs")) {
        if (!k.contains(QStringLiteral("host")))
            return false;
        const bool ssl = type == QLatin1String("davs") || k.value(QStringLiteral("ssl")) == QLatin1String("true");
        m.scheme = ssl ? QStringLiteral("davs") : QStringLiteral("dav");
        QString prefix = k.value(QStringLiteral("prefix"));
        if (!prefix.startsWith(QLatin1Char('/')))
            prefix.prepend(QLatin1Char('/'));
        if (!prefix.endsWith(QLatin1Char('/')))
            prefix += QLatin1Char('/');
        m.address = m.scheme + QStringLiteral("://") + hostPart(k, QStringLiteral("host")) + prefix;
        m.name = k.value(QStringLiteral("host")) + (prefix.size() > 1 ? prefix.chopped(1) : QString());
    } else if (type == QLatin1String("mtp") || type == QLatin1String("gphoto2")) {
        if (!k.contains(QStringLiteral("host")))
            return false;
        m.scheme = type;
        m.address = type + QStringLiteral("://") + k.value(QStringLiteral("host")) + QLatin1Char('/');
        // "Google_Pixel_7_1A2B3C" -> "Google Pixel 7": the serial is the last part.
        QStringList parts = k.value(QStringLiteral("host")).split(QLatin1Char('_'), Qt::SkipEmptyParts);
        if (parts.size() > 1 && parts.last().size() >= 6)
            parts.removeLast();
        m.name = parts.join(QLatin1Char(' '));
    } else {
        return false; // google-drive, archive mounts…: not offered here
    }
    *mount = m;
    return true;
}

QList<Mount> mounts()
{
    QList<Mount> list;
    const QString root = gvfsRoot();
    if (root.isEmpty())
        return list;
    const QStringList dirs = QDir(root).entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QString &dir : dirs) {
        Mount m;
        if (parseMountDir(dir, &m)) {
            m.localPath = root + QLatin1Char('/') + dir;
            list.append(m);
        }
    }
    return list;
}

Mount mountOf(const QString &path)
{
    const QString root = gvfsRoot();
    if (root.isEmpty() || !path.startsWith(root + QLatin1Char('/')))
        return {};
    const QString rest = path.mid(root.size() + 1);
    const QString dir = rest.section(QLatin1Char('/'), 0, 0);
    Mount m;
    if (!parseMountDir(dir, &m))
        return {};
    m.localPath = root + QLatin1Char('/') + dir;
    return m;
}

// --------------------------------------------------------------- Mounter ---

Mounter::Mounter(const QString &address, QObject *parent) : QObject(parent), m_address(address.trimmed()) {}

Mounter::~Mounter()
{
    if (m_process) {
        m_process->disconnect(this);
        m_process->kill();
        m_process->waitForFinished(1000);
    }
}

void Mounter::start(bool anonymous)
{
    if (m_process) {
        m_process->disconnect(this);
        m_process->kill();
        m_process->deleteLater();
    }
    m_buffer.clear();
    m_message.clear();
    m_haveLogin = false;
    m_askedOnce = false;
    m_pendingPrompt.clear();
    m_error.clear();
    m_done = false;

    auto *process = new QProcess(this);
    m_process = process;
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("LC_ALL"), QStringLiteral("C.UTF-8")); // prompts in English, to recognise them
    process->setProcessEnvironment(env);
    connect(process, &QProcess::readyReadStandardOutput, this, &Mounter::readOutput);
    connect(process, &QProcess::finished, this, [this](int code) { processFinished(code); });
    connect(process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart && !m_done) {
            m_done = true;
            emit finished(false, QString(), tr("The gio tool isn't installed"));
        }
    });
    QStringList args{QStringLiteral("mount")};
    if (anonymous)
        args << QStringLiteral("--anonymous");
    args << m_address;
    process->start(Tools::gio(), args);
}

void Mounter::write(const QString &line)
{
    if (m_process)
        m_process->write(line.toUtf8() + '\n');
}

void Mounter::readOutput()
{
    if (!m_process)
        return;
    m_buffer += m_process->readAllStandardOutput();
    const QString text = QString::fromUtf8(m_buffer);

    // A prompt is the last line, without a newline yet: "User [me]: ".
    const qsizetype lastNewline = text.lastIndexOf(QLatin1Char('\n'));
    const QString tail = text.mid(lastNewline + 1);
    if (!tail.endsWith(QLatin1String(": ")))
        return; // more to come
    const QString before = lastNewline < 0 ? QString() : text.left(lastNewline);

    static const QRegularExpression userRe(QStringLiteral("^User(?: \\[(.*)\\])?: $"));
    static const QRegularExpression domainRe(QStringLiteral("^Domain(?: \\[(.*)\\])?: $"));
    static const QRegularExpression passwordRe(QStringLiteral("^Password: $"));
    static const QRegularExpression choiceRe(QStringLiteral("^Choice(?: \\[.*\\])?: $"));
    static const QRegularExpression choiceLine(QStringLiteral("^\\[(\\d+)\\] (.*)$"));

    // The explanation gio printed before the prompts (minus "Authentication Required").
    QStringList lines;
    for (const QString &l : before.split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
        if (l.trimmed() != QLatin1String("Authentication Required") && !choiceLine.match(l).hasMatch())
            lines << l.trimmed();
    }
    if (!lines.isEmpty())
        m_message = lines.join(QLatin1Char('\n'));

    const auto user = userRe.match(tail);
    const auto domain = domainRe.match(tail);
    if (user.hasMatch() || (passwordRe.match(tail).hasMatch() && !m_haveLogin)) {
        if (!m_haveLogin || m_askedOnce) {
            // First time, or asked again: the last answer was wrong.
            const bool retry = m_askedOnce;
            m_haveLogin = false;
            m_askedOnce = false;
            m_pendingPrompt = tail;
            const bool smb = m_address.startsWith(QLatin1String("smb"), Qt::CaseInsensitive);
            emit needLogin(m_message, user.hasMatch(), user.captured(1), smb, QStringLiteral("WORKGROUP"), retry);
            m_buffer.clear();
            return;
        }
    }
    m_buffer.clear();
    if (user.hasMatch()) {
        write(m_login.user.isEmpty() ? user.captured(1) : m_login.user);
    } else if (domain.hasMatch()) {
        write(m_login.domain.isEmpty() ? domain.captured(1) : m_login.domain);
    } else if (passwordRe.match(tail).hasMatch()) {
        write(m_login.password);
        m_askedOnce = true; // if gio asks again, the answer was wrong
    } else if (choiceRe.match(tail).hasMatch()) {
        QStringList choices;
        QList<int> numbers;
        for (const QString &l : before.split(QLatin1Char('\n'))) {
            const auto m = choiceLine.match(l.trimmed());
            if (m.hasMatch()) {
                numbers << m.captured(1).toInt();
                choices << m.captured(2);
            }
        }
        m_pendingPrompt = tail;
        emit question(m_message, choices, numbers);
    } else {
        write(QString()); // unknown prompt: take gio's default
    }
}

void Mounter::answer(const Login &login)
{
    m_login = login;
    m_haveLogin = true;
    const QString prompt = m_pendingPrompt;
    m_pendingPrompt.clear();
    // Answer the prompt that is waiting; the others as they come.
    if (prompt.startsWith(QLatin1String("User"))) {
        static const QRegularExpression userRe(QStringLiteral("^User(?: \\[(.*)\\])?: $"));
        write(login.user.isEmpty() ? userRe.match(prompt).captured(1) : login.user);
    } else if (prompt.startsWith(QLatin1String("Password"))) {
        write(login.password);
        m_askedOnce = true;
    }
}

void Mounter::choose(int choice)
{
    m_pendingPrompt.clear();
    write(QString::number(choice));
}

void Mounter::cancel()
{
    if (m_process) {
        m_process->disconnect(this);
        m_process->kill();
        m_process->waitForFinished(1000);
        m_process->deleteLater();
        m_process = nullptr;
    }
    if (!m_done) {
        m_done = true;
        emit finished(false, QString(), QString()); // no message: the user cancelled
    }
}

void Mounter::processFinished(int code)
{
    if (!m_process || m_done)
        return;
    QString err = QString::fromUtf8(m_process->readAllStandardError()).trimmed();
    m_process->deleteLater();
    m_process = nullptr;
    // "gio: smb://x/: Failed to mount Windows share: Permission denied"
    static const QRegularExpression prefix(QStringLiteral("^gio: [^ ]+: "));
    err = err.section(QLatin1Char('\n'), 0, 0).remove(prefix);
    const bool already = err.contains(QLatin1String("already mounted"), Qt::CaseInsensitive);
    if (code == 0 || already) {
        resolveLocalPath();
        return;
    }
    m_done = true;
    if (err.isEmpty())
        err = tr("Couldn't connect to %1").arg(m_address);
    emit finished(false, QString(), err);
}

// `gio info <address>` says where its folder is ("local path: …").
void Mounter::resolveLocalPath()
{
    auto *info = new QProcess(this);
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("LC_ALL"), QStringLiteral("C.UTF-8"));
    info->setProcessEnvironment(env);
    connect(info, &QProcess::finished, this, [this, info] {
        QString local;
        const QStringList lines = QString::fromUtf8(info->readAllStandardOutput()).split(QLatin1Char('\n'));
        for (const QString &l : lines) {
            if (l.startsWith(QLatin1String("local path: ")))
                local = QDir::cleanPath(l.mid(12).trimmed());
        }
        info->deleteLater();
        if (m_done)
            return;
        m_done = true;
        emit finished(true, local, QString());
    });
    connect(info, &QProcess::errorOccurred, this, [this, info](QProcess::ProcessError e) {
        if (e != QProcess::FailedToStart || m_done)
            return;
        info->deleteLater();
        m_done = true;
        emit finished(true, QString(), QString());
    });
    info->start(Tools::gio(), {QStringLiteral("info"), m_address});
}

void unmount(const QString &address, QObject *context, const std::function<void(bool, const QString &)> &done)
{
    auto *process = new QProcess(context);
    QObject::connect(process, &QProcess::finished, context, [process, done](int code, QProcess::ExitStatus status) {
        static const QRegularExpression prefix(QStringLiteral("^gio: [^ ]+: "));
        QString err = QString::fromUtf8(process->readAllStandardError()).trimmed().section(QLatin1Char('\n'), 0, 0);
        err.remove(prefix);
        process->deleteLater();
        done(status == QProcess::NormalExit && code == 0, err);
    });
    QObject::connect(process, &QProcess::errorOccurred, context, [process, done](QProcess::ProcessError e) {
        if (e != QProcess::FailedToStart)
            return;
        process->deleteLater();
        done(false, QObject::tr("The gio tool isn't installed"));
    });
    process->start(Tools::gio(), {QStringLiteral("mount"), QStringLiteral("-u"), address});
}

} // namespace Remote
