#pragma once

#include <QDialog>

class QCheckBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;

// File → Connect to Server…: an address (sftp://, smb://, ftp://, dav://…)
// and the servers used before (network/recent, newest first).
class ConnectDialog : public QDialog
{
    Q_OBJECT

public:
    explicit ConnectDialog(QWidget *parent = nullptr);
    QString address() const;

    // The recent list, newest first (at most 10).
    static QStringList recentServers();
    static void rememberServer(const QString &address);
    static void forgetServer(const QString &address);

private:
    void validate();

    QLineEdit *m_address = nullptr;
    QListWidget *m_recent = nullptr;
    QLabel *m_problem = nullptr;
    QPushButton *m_connect = nullptr;
};

// The login a server asks for: user (and domain for Windows shares) and
// password, or anonymous where servers allow it.
class LoginDialog : public QDialog
{
    Q_OBJECT

public:
    LoginDialog(const QString &address, const QString &message, bool askUser, const QString &defaultUser,
                bool askDomain, const QString &defaultDomain, bool retry, QWidget *parent = nullptr);

    bool anonymous() const;
    QString user() const;
    QString domain() const;
    QString password() const;

private:
    QCheckBox *m_anonymous = nullptr;
    QLineEdit *m_user = nullptr;
    QLineEdit *m_domain = nullptr;
    QLineEdit *m_password = nullptr;
};
