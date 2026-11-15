#pragma once

#include <QObject>
#include <QString>

class QWidget;

// Connecting to a server, disconnecting from one and ejecting a drive, with
// the questions these ask (login, an unknown host key, which shared folder).
// The work itself is GVfs's (`gio`, see Remote); this class runs the
// conversation and reports through signals, so the window only decides what
// to do with the result.
class MountActions : public QObject
{
    Q_OBJECT

public:
    explicit MountActions(QWidget *window); // dialogs open over `window`

    // Mounts `address` ("sftp://host/path", "smb://server/"…). A server
    // without a folder of its own (smb://server/) asks which share.
    void connectToServer(const QString &address);
    // Disconnects the server mounted at `root`, or unmounts and powers off
    // the drive there. The caller makes sure nothing of ours uses it.
    void eject(const QString &root, const QString &name);

signals:
    void message(const QString &text);        // progress and errors, for the floating bar
    void connected(const QString &localPath); // the server's folder: go there
    void mountsChanged();                     // something was mounted or removed

private:
    void chooseShare(const QString &serverAddress);

    QWidget *m_window;
};
