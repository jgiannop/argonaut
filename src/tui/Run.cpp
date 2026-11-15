#include "Run.h"

#include "App.h"
#include "Screen.h"
#include "Terminal.h"

#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>
#include <QSocketNotifier>
#include <QTimer>

#include <cstdio>
#include <memory>
#include <unistd.h>

namespace Tui {

int run(int argc, char **argv)
{
    QStringList folders;
    for (int i = 1; i < argc; ++i) {
        const QString arg = QString::fromLocal8Bit(argv[i]);
        if (arg == QLatin1String("--help") || arg == QLatin1String("-h")) {
            std::printf("Usage: argonaut --tui [left folder] [right folder]\n"
                        "Argonaut in the terminal: two panes of files. Press F1 inside for the keys.\n");
            return 0;
        }
        if (!arg.startsWith(QLatin1Char('-')))
            folders << QFileInfo(arg).absoluteFilePath();
    }
    if (!::isatty(STDIN_FILENO) || !::isatty(STDOUT_FILENO)) {
        std::fprintf(stderr, "argonaut: the terminal interface needs a terminal (not a pipe or a file).\n");
        return 2;
    }
    // The engine is Qt's, but nothing is drawn in a window: no display is
    // needed (a server over SSH has none).
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("Argonaut")); // the window's settings and extensions
    QCoreApplication::setOrganizationName(QStringLiteral("Argonaut"));
    QCoreApplication::setApplicationVersion(QStringLiteral(ARGONAUT_VERSION));

    App tui(folders.value(0, QDir::currentPath()), folders.value(1, QDir::homePath()));
    auto terminal = std::make_unique<Terminal>();
    Screen screen;
    bool dirty = true;
    auto draw = [&] {
        if (!dirty)
            return;
        dirty = false;
        if (screen.width() != terminal->width() || screen.height() != terminal->height())
            screen.resize(terminal->width(), terminal->height());
        tui.render(screen);
        terminal->show(screen);
    };
    // Many changes in a row (a folder loading) are one frame.
    QTimer frame;
    frame.setSingleShot(true);
    frame.setInterval(15);
    QObject::connect(&frame, &QTimer::timeout, &app, draw);
    QObject::connect(&tui, &App::changed, &app, [&] {
        dirty = true;
        if (!frame.isActive())
            frame.start();
    });
    QObject::connect(&tui, &App::quitRequested, &app, &QCoreApplication::quit);

    auto readKeys = [&] {
        bool resized = false;
        const QList<Key> keys = terminal->readKeys(&resized);
        for (const Key &key : keys)
            tui.key(key);
        const QList<Mouse> clicks = terminal->takeMouse();
        for (const Mouse &click : clicks)
            tui.mouse(click);
        if (resized || !keys.isEmpty() || !clicks.isEmpty()) {
            dirty = true;
            draw();
        }
    };
    QSocketNotifier input(STDIN_FILENO, QSocketNotifier::Read);
    QObject::connect(&input, &QSocketNotifier::activated, &app, readKeys);

    // A text file to read: the terminal goes to `less` until it is closed.
    // It only shows the file: no helper programs are run on it (LESSOPEN),
    // and from inside it nothing can be started or written (LESSSECURE).
    QObject::connect(&tui, &App::viewRequested, &app, [&](const QString &path) {
        QString pager = QStandardPaths::findExecutable(QStringLiteral("less"));
        if (pager.isEmpty())
            pager = QStandardPaths::findExecutable(QStringLiteral("more"));
        if (pager.isEmpty()) {
            tui.notify(QStringLiteral("Neither `less` nor `more` is installed to show the file with"));
            return;
        }
        input.setEnabled(false);
        terminal->suspend();
        QProcess viewer;
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.remove(QStringLiteral("LESSOPEN"));
        env.remove(QStringLiteral("LESSCLOSE"));
        env.insert(QStringLiteral("LESSSECURE"), QStringLiteral("1"));
        viewer.setProcessEnvironment(env);
        viewer.setProcessChannelMode(QProcess::ForwardedChannels);
        viewer.setInputChannelMode(QProcess::ForwardedInputChannel);
        viewer.start(pager, {QStringLiteral("--"), path});
        viewer.waitForFinished(-1);
        terminal->resume();
        input.setEnabled(true);
        dirty = true;
        draw();
    });
    // A resize is not input: looked for now and then. Also the progress of
    // a running operation, which says nothing when a big file is copied.
    QTimer poll;
    poll.setInterval(120);
    QObject::connect(&poll, &QTimer::timeout, &app, [&] {
        readKeys();
        if (tui.isBusy()) {
            dirty = true;
            draw();
        }
    });
    poll.start();

    draw();
    const int code = app.exec();
    terminal.reset(); // the terminal back as it was, before anything is printed
    return code;
}

} // namespace Tui
