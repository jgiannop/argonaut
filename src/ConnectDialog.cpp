#include "ConnectDialog.h"

#include "Remote.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSettings>
#include <QUrl>
#include <QVBoxLayout>

ConnectDialog::ConnectDialog(QWidget *parent) : QDialog(parent)
{
    setObjectName("connectDialog");
    setWindowTitle(tr("Connect to Server"));
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(24, 20, 24, 20);
    layout->setSpacing(8);

    auto *heading = new QLabel(tr("Server address"), this);
    heading->setObjectName("prefHeading");
    layout->addWidget(heading);
    m_address = new QLineEdit(this);
    m_address->setObjectName("connectAddress");
    m_address->setPlaceholderText(QStringLiteral("sftp://example.com"));
    m_address->setClearButtonEnabled(true);
    layout->addWidget(m_address);
    auto *hint = new QLabel(tr("For example <b>sftp://me@example.com</b> (SSH), <b>smb://nas/photos</b> "
                               "(Windows share), <b>ftp://ftp.example.org</b>, or <b>davs://cloud.example.com/dav</b> "
                               "(WebDAV)."),
                            this);
    hint->setObjectName("prefHint");
    hint->setWordWrap(true);
    layout->addWidget(hint);
    m_problem = new QLabel(this);
    m_problem->setObjectName("propDanger");
    m_problem->hide();
    layout->addWidget(m_problem);

    const QStringList recent = recentServers();
    if (!recent.isEmpty()) {
        layout->addSpacing(10);
        auto *recentHeading = new QLabel(tr("Recent servers"), this);
        recentHeading->setObjectName("prefHeading");
        layout->addWidget(recentHeading);
        m_recent = new QListWidget(this);
        m_recent->setObjectName("appList");
        m_recent->addItems(recent);
        m_recent->setMaximumHeight(160);
        layout->addWidget(m_recent);
        connect(m_recent, &QListWidget::currentTextChanged, m_address, &QLineEdit::setText);
        connect(m_recent, &QListWidget::itemDoubleClicked, this, [this] {
            if (m_connect->isEnabled())
                accept();
        });
    }

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
    m_connect = buttons->addButton(tr("Connect"), QDialogButtonBox::AcceptRole);
    m_connect->setDefault(true);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addSpacing(8);
    layout->addWidget(buttons);

    connect(m_address, &QLineEdit::textChanged, this, &ConnectDialog::validate);
    validate();
    resize(480, sizeHint().height());
}

QString ConnectDialog::address() const
{
    QString text = m_address->text().trimmed();
    if (!text.isEmpty() && !text.contains(QStringLiteral("://")))
        text.prepend(QStringLiteral("sftp://")); // a bare host name: SSH, like Nautilus
    return text;
}

void ConnectDialog::validate()
{
    const QString text = address();
    QString problem;
    if (!text.isEmpty() && !Remote::isRemoteAddress(text))
        problem = tr("Kosmos can connect with sftp, smb, ftp, ftps, dav, davs, afp and nfs.");
    else if (!text.isEmpty() && QUrl(text).host().isEmpty() && !text.startsWith(QLatin1String("mtp")))
        problem = tr("The address needs a server name.");
    m_problem->setText(problem);
    m_problem->setVisible(!problem.isEmpty());
    m_connect->setEnabled(!text.isEmpty() && problem.isEmpty());
}

QStringList ConnectDialog::recentServers()
{
    return QSettings().value(QStringLiteral("network/recent")).toStringList();
}

void ConnectDialog::rememberServer(const QString &address)
{
    QStringList list = recentServers();
    list.removeAll(address);
    list.prepend(address);
    while (list.size() > 10)
        list.removeLast();
    QSettings().setValue(QStringLiteral("network/recent"), list);
}

void ConnectDialog::forgetServer(const QString &address)
{
    QStringList list = recentServers();
    list.removeAll(address);
    QSettings().setValue(QStringLiteral("network/recent"), list);
}

// -------------------------------------------------------------- LoginDialog

LoginDialog::LoginDialog(const QString &address, const QString &message, bool askUser, const QString &defaultUser,
                         bool askDomain, const QString &defaultDomain, bool retry, QWidget *parent)
    : QDialog(parent)
{
    setObjectName("connectDialog");
    setWindowTitle(tr("Log In"));
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(24, 20, 24, 20);
    layout->setSpacing(8);
    auto *heading = new QLabel(tr("Log in to %1").arg(QUrl(address).host().isEmpty() ? address : QUrl(address).host()),
                               this);
    heading->setObjectName("prefHeading");
    layout->addWidget(heading);
    auto *text = new QLabel(message.isEmpty() ? address : message, this);
    text->setObjectName("prefHint");
    text->setWordWrap(true);
    layout->addWidget(text);
    if (retry) {
        auto *wrong = new QLabel(tr("That user name or password didn't work. Try again."), this);
        wrong->setObjectName("propDanger");
        layout->addWidget(wrong);
    }

    // Anonymous: for servers that allow it (smb, ftp, WebDAV), not ssh.
    const bool canAnonymous = !address.startsWith(QLatin1String("sftp")) && !address.startsWith(QLatin1String("ssh"));
    if (canAnonymous) {
        m_anonymous = new QCheckBox(tr("Connect anonymously"), this);
        m_anonymous->setObjectName("prefCheck");
        layout->addWidget(m_anonymous);
    }

    auto *form = new QFormLayout;
    form->setContentsMargins(0, 6, 0, 0);
    if (askUser) {
        m_user = new QLineEdit(defaultUser, this);
        m_user->setObjectName("connectAddress");
        form->addRow(tr("User"), m_user);
    }
    if (askDomain) {
        m_domain = new QLineEdit(defaultDomain, this);
        m_domain->setObjectName("connectAddress");
        form->addRow(tr("Domain"), m_domain);
    }
    m_password = new QLineEdit(this);
    m_password->setObjectName("connectAddress");
    m_password->setEchoMode(QLineEdit::Password);
    form->addRow(tr("Password"), m_password);
    layout->addLayout(form);

    if (m_anonymous) {
        connect(m_anonymous, &QCheckBox::toggled, this, [this](bool on) {
            for (QLineEdit *edit : {m_user, m_domain, m_password}) {
                if (edit)
                    edit->setEnabled(!on);
            }
        });
    }

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
    QPushButton *login = buttons->addButton(tr("Connect"), QDialogButtonBox::AcceptRole);
    login->setDefault(true);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addSpacing(8);
    layout->addWidget(buttons);
    (m_user && defaultUser.isEmpty() ? m_user : m_password)->setFocus();
    resize(420, sizeHint().height());
}

bool LoginDialog::anonymous() const
{
    return m_anonymous && m_anonymous->isChecked();
}

QString LoginDialog::user() const
{
    return m_user ? m_user->text() : QString();
}

QString LoginDialog::domain() const
{
    return m_domain ? m_domain->text() : QString();
}

QString LoginDialog::password() const
{
    return m_password->text();
}
