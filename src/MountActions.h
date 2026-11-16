#pragma once

#include <QObject>
#include <QString>

class QWidget;

// Ejecting a drive (unmounting it and powering it off, with GNOME's own
// tool), and disconnecting a server some other way than through its
// extension. Reports through signals, so the window only decides what to do
// with the result. Connecting to servers is the Connect to Server
// extension's (extensions/connect).
class MountActions : public QObject
{
    Q_OBJECT

public:
    explicit MountActions(QWidget *window);

    // Disconnects the server mounted at `root`, or unmounts and powers off
    // the drive there. The caller makes sure nothing of ours uses it.
    void eject(const QString &root, const QString &name);

signals:
    void message(const QString &text);        // progress and errors, for the floating bar
    void mountsChanged();                     // something was mounted or removed

private:
    QWidget *m_window;
};
