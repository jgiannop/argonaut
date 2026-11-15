#include "MountActions.h"

#include "ConnectDialog.h"
#include "Remote.h"
#include "Tools.h"

#include <QAbstractButton>
#include <QInputDialog>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QUrl>
#include <QWidget>

MountActions::MountActions(QWidget *window) : QObject(window), m_window(window) {}

void MountActions::connectToServer(const QString &address)
{
#if !defined(Q_OS_LINUX)
    emit message(tr("Connecting to servers needs GVfs (Linux)"));
    Q_UNUSED(address);
    return;
#else
    QString clean = address.trimmed();
    if (clean.startsWith(QLatin1String("ssh://")))
        clean.replace(0, 3, QStringLiteral("sftp")); // GVfs knows it as sftp
    auto *mounter = new Remote::Mounter(clean, this);
    emit message(tr("Connecting to %1…").arg(QUrl(clean).host().isEmpty() ? clean : QUrl(clean).host()));
    connect(mounter, &Remote::Mounter::needLogin, this,
            [this, mounter](const QString &message, bool askUser, const QString &defaultUser, bool askDomain,
                            const QString &defaultDomain, bool retry) {
                LoginDialog dialog(mounter->address(), message, askUser, defaultUser, askDomain, defaultDomain, retry, m_window);
                if (dialog.exec() != QDialog::Accepted) {
                    mounter->cancel();
                    return;
                }
                if (dialog.anonymous()) {
                    mounter->start(true);
                    return;
                }
                mounter->answer({dialog.user(), dialog.domain(), dialog.password()});
            });
    connect(mounter, &Remote::Mounter::question, this,
            [this, mounter](const QString &message, const QStringList &choices, const QList<int> &numbers) {
                // E.g. an unknown SSH host key: "Log In Anyway" / "Cancel Login".
                QMessageBox box(QMessageBox::Question, tr("Connect to Server"), message, QMessageBox::NoButton, m_window);
                QList<QAbstractButton *> buttons;
                for (const QString &choice : choices)
                    buttons << box.addButton(choice, QMessageBox::AcceptRole);
                box.exec();
                const int at = int(buttons.indexOf(box.clickedButton()));
                if (at < 0) {
                    mounter->cancel();
                    return;
                }
                mounter->choose(numbers.value(at, at));
            });
    connect(mounter, &Remote::Mounter::finished, this, [this, mounter, clean](bool ok, const QString &local, const QString &error) {
        mounter->deleteLater();
        emit mountsChanged();
        if (!ok) {
            if (!error.isEmpty())
                emit message(error);
            return;
        }
        ConnectDialog::rememberServer(clean);
        if (local.isEmpty()) {
            chooseShare(clean); // smb://server/: a list of shares, no folder
            return;
        }
        emit connected(local);
    });
    mounter->start();
#endif
}

// smb://server/ has no folder of its own: pick one of its shares.
void MountActions::chooseShare(const QString &serverAddress)
{
    auto *list = new QProcess(this);
    connect(list, &QProcess::finished, this, [this, list, serverAddress] {
        QStringList shares;
        for (const QString &line : QString::fromUtf8(list->readAllStandardOutput()).split(QLatin1Char('\n'))) {
            const QString name = line.trimmed();
            if (!name.isEmpty() && !name.endsWith(QLatin1Char('$'))) // hidden admin shares (C$, IPC$)
                shares << name;
        }
        list->deleteLater();
        if (shares.isEmpty()) {
            emit message(tr("%1 has no shared folders").arg(QUrl(serverAddress).host()));
            return;
        }
        bool ok = false;
        const QString share = QInputDialog::getItem(m_window, tr("Shared Folders"),
                                                    tr("Shared folders on %1:").arg(QUrl(serverAddress).host()), shares,
                                                    0, false, &ok);
        if (!ok)
            return;
        QString base = serverAddress;
        if (!base.endsWith(QLatin1Char('/')))
            base += QLatin1Char('/');
        connectToServer(base + QString::fromUtf8(QUrl::toPercentEncoding(share)));
    });
    list->start(Tools::gio(), {QStringLiteral("list"), serverAddress});
}

// A server is disconnected through GVfs; a drive is unmounted and powered
// off by GNOME's own tool, which does it safely.
void MountActions::eject(const QString &root, const QString &name)
{
    if (const Remote::Mount server = Remote::mountOf(root); !server.localPath.isEmpty()) {
        emit message(tr("Disconnecting from “%1”…").arg(name));
        Remote::unmount(server.address, this, [this, name](bool ok, const QString &error) {
            emit message(ok ? tr("Disconnected from “%1”").arg(name)
                           : tr("Couldn't disconnect from “%1”: %2").arg(name, error));
            emit mountsChanged();
        });
        return;
    }
    const QString program = Tools::program("ARGONAUT_EJECT_PROGRAM", QStringLiteral("gio"));
    auto *process = new QProcess(this);
    connect(process, &QProcess::finished, this, [this, process, name](int code, QProcess::ExitStatus status) {
        if (status == QProcess::NormalExit && code == 0) {
            emit message(tr("“%1” can be safely removed").arg(name));
        } else {
            QString why = QString::fromLocal8Bit(process->readAllStandardError()).trimmed();
            why = why.section(QLatin1Char('\n'), 0, 0);
            if (why.contains(QLatin1String("busy"), Qt::CaseInsensitive))
                why = tr("it's in use by another program or the terminal");
            emit message(why.isEmpty() ? tr("Couldn't eject “%1”").arg(name)
                                      : tr("Couldn't eject “%1”: %2").arg(name, why));
        }
        emit mountsChanged();
        process->deleteLater();
    });
    connect(process, &QProcess::errorOccurred, this, [this, process, name](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart) {
            emit message(tr("Couldn't eject “%1”: the gio tool isn't installed").arg(name));
            process->deleteLater();
        }
    });
    emit message(tr("Ejecting “%1”…").arg(name));
    process->start(program, {QStringLiteral("mount"), QStringLiteral("--eject"), root});
}
