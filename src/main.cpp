#include "MainWindow.h"
#include "Theme.h"

#include <QApplication>
#include <QDir>
#include <QUrl>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    // Same base style everywhere; our style sheet and drawing do the rest.
    QApplication::setStyle("Fusion");
    QApplication::setApplicationName("Kaftilous");
    QApplication::setApplicationVersion("0.1.0");
    QApplication::setOrganizationName("Kaftilous"); // QSettings location

    // Light / dark / follow system, from Preferences.
    applyThemeMode(savedThemeMode());

    MainWindow window;

    // Open a folder given on the command line (path or file:// URL).
    const QStringList args = QApplication::arguments();
    QString start = QDir::homePath();
    if (args.size() > 1) {
        const QUrl url = QUrl::fromUserInput(args.at(1), QDir::currentPath(),
                                             QUrl::AssumeLocalFile);
        if (url.isLocalFile())
            start = url.toLocalFile();
    }
    window.navigateTo(start);

    window.show();
    return app.exec();
}
