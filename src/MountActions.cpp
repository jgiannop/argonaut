#include "MountActions.h"

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
