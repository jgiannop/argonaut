#include "DefaultFileManager.h"
#include "MainWindow.h"
#include "PropertiesDialog.h"
#include "Theme.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QFileInfo>
#include <QTimer>
#include <QUrl>

#include <cstdio>

#if defined(KAFTILOUS_DBUS)
#include "FileManagerService.h"
#include <QDBusConnection>
#endif

namespace {

void setIdentity()
{
    QCoreApplication::setApplicationName("Kaftilous");
    QCoreApplication::setApplicationVersion("0.1.0");
    QCoreApplication::setOrganizationName("Kaftilous"); // QSettings location
}

// Paths or URLs from the command line, split into folders and other items.
void sortLocations(const QStringList &args, QStringList *folders, QStringList *items)
{
    for (const QString &arg : args) {
        const QUrl url = QUrl::fromUserInput(arg, QDir::currentPath(), QUrl::AssumeLocalFile);
        if (!url.isLocalFile())
            continue;
        const QString path = QDir::cleanPath(url.toLocalFile());
        if (QFileInfo(path).isDir())
            folders->append(path);
        else if (QFileInfo::exists(path))
            items->append(path); // a file: show it selected in its folder
    }
}

// --make-default / --restore-nautilus: no window, so they also work from a
// text console or over SSH, even if the app itself won't start.
int switchDefault(bool makeDefault)
{
    QString error;
    QString restoredTo;
    const bool ok = makeDefault ? DefaultFileManager::makeDefault(&error)
                                : DefaultFileManager::restorePrevious(&error, &restoredTo);
    if (!ok) {
        std::fprintf(stderr, "kaftilous: %s\n", qPrintable(error));
        return 1;
    }
    if (makeDefault)
        std::printf("Kaftilous now opens folders and \"Show in folder\".\n"
                    "To go back: kaftilous --restore-nautilus\n");
    else
        std::printf("Folders are opened by %s again.\n", qPrintable(restoredTo));
    return 0;
}

MainWindow *newWindow()
{
    auto *window = new MainWindow;
    window->setAttribute(Qt::WA_DeleteOnClose);
    return window;
}

void present(QWidget *window, const QString &startupId)
{
    // Lets the window come to the front when another app asked for it.
    if (!startupId.isEmpty()) {
        qputenv("XDG_ACTIVATION_TOKEN", startupId.toUtf8()); // Wayland
        qputenv("DESKTOP_STARTUP_ID", startupId.toUtf8());   // X11
    }
    window->show();
    window->raise();
    window->activateWindow();
}

} // namespace

int main(int argc, char *argv[])
{
    // The switches that need no window come first, before a display is needed.
    for (int i = 1; i < argc; ++i) {
        const QByteArray arg(argv[i]);
        if (arg == "--make-default" || arg == "--restore-nautilus" || arg == "--restore-default") {
            QCoreApplication core(argc, argv);
            setIdentity();
            return switchDefault(arg == "--make-default");
        }
    }

    QApplication app(argc, argv);
    setIdentity();
    QGuiApplication::setDesktopFileName("kaftilous"); // Wayland app id: the dock finds our icon
    QApplication::setWindowIcon(applicationIcon());
    // Same base style everywhere; our style sheet and drawing do the rest.
    QApplication::setStyle("Fusion");
    // Light / dark / follow system, from Preferences.
    applyThemeMode(savedThemeMode());

    QCommandLineParser parser;
    parser.setApplicationDescription(QApplication::translate("main", "A fast file manager."));
    parser.addHelpOption();
    parser.addVersionOption();
    const QCommandLineOption service("service", QApplication::translate(
        "main", "Answer \"Show in folder\" for other apps (started by D-Bus)."));
    parser.addOption(service);
    parser.addOption({"make-default", QApplication::translate("main", "Make Kaftilous the file manager for folders and \"Show in folder\".")});
    parser.addOption({{"restore-nautilus", "restore-default"},
                      QApplication::translate("main", "Give folders back to Nautilus (or the previous file manager).")});
    parser.addPositionalArgument("locations", QApplication::translate("main", "Folders to open, or files to show."),
                                 "[locations…]");
    parser.process(app);

#if defined(KAFTILOUS_DBUS)
    if (parser.isSet(service)) {
        // D-Bus started us for org.freedesktop.FileManager1. Windows open on
        // request; we quit when the last one closes, or soon if none was asked for.
        FileManagerService manager;
        if (!manager.registerOn(QDBusConnection::sessionBus())) {
            std::fprintf(stderr, "kaftilous: another program already answers \"Show in folder\".\n");
            return 1;
        }
        QObject::connect(&manager, &FileManagerService::showFolders, [](const QStringList &paths, const QString &id) {
            if (paths.isEmpty())
                return;
            MainWindow *window = newWindow();
            window->openFolders(paths);
            present(window, id);
        });
        QObject::connect(&manager, &FileManagerService::showItems, [](const QStringList &paths, const QString &id) {
            if (paths.isEmpty())
                return;
            MainWindow *window = newWindow();
            window->showItems(paths);
            present(window, id);
        });
        QObject::connect(&manager, &FileManagerService::showItemProperties,
                         [](const QStringList &paths, const QString &id) {
                             if (paths.isEmpty())
                                 return;
                             auto *dialog = new PropertiesDialog(paths, nullptr);
                             dialog->setAttribute(Qt::WA_DeleteOnClose);
                             present(dialog, id);
                         });
        QTimer::singleShot(30000, &app, [] {
            if (QApplication::topLevelWindows().isEmpty())
                QApplication::quit(); // nothing was asked for
        });
        return app.exec();
    }
#else
    Q_UNUSED(service)
#endif

    // Folders open (the first here, the others in tabs); files are shown
    // selected in their folder.
    QStringList folders;
    QStringList items;
    sortLocations(parser.positionalArguments(), &folders, &items);
    MainWindow window;
    if (!folders.isEmpty()) {
        for (const QString &item : std::as_const(items)) // files' folders: more tabs
            if (!folders.contains(QFileInfo(item).absolutePath()))
                folders << QFileInfo(item).absolutePath();
        window.openFolders(folders);
    } else if (!items.isEmpty()) {
        window.showItems(items);
    } else {
        window.navigateTo(QDir::homePath());
    }
    window.show();
    return app.exec();
}
