#pragma once

#include <QDialog>

class QStackedWidget;

// Help → About Argonaut: the app's icon, name and version, then three pages:
// About (what it is), System (versions, features and paths, with Copy for
// bug reports) and Credits (what it is built with).
class AboutDialog : public QDialog
{
    Q_OBJECT

public:
    explicit AboutDialog(QWidget *parent = nullptr);

    // The System page as plain text, for "Copy Details".
    static QString systemDetails();

private:
    QWidget *buildAboutPage();
    QWidget *buildSystemPage();
    QWidget *buildCreditsPage();

    QStackedWidget *m_pages = nullptr;
};
