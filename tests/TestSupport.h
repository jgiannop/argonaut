#pragma once

// Shared by the test programs: the sandbox every test runs in, and waiting
// for a file job.

#include "FileJob.h"
#include "FileOperations.h"

#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QObject>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>

namespace TestSupport {

// A private HOME and XDG tree in a temporary folder, so a test never reads or
// changes the real settings, bookmarks, stars, recent files, cache or Trash.
// Make it first thing in main(), before QApplication: Qt reads these
// variables once.
class Sandbox
{
public:
    Sandbox()
    {
        const QString root = m_dir.path();
        for (const auto &[variable, folder] : {std::pair{"HOME", "/home"},
                                               std::pair{"XDG_DATA_HOME", "/data"},
                                               std::pair{"XDG_CONFIG_HOME", "/config"},
                                               std::pair{"XDG_CACHE_HOME", "/cache"}}) {
            QDir().mkpath(root + QLatin1String(folder));
            qputenv(variable, QFile::encodeName(root + QLatin1String(folder)));
        }
        // Never look at (or empty) the trash folders of real mounted drives.
        qputenv("ARGONAUT_TRASH_HOME_ONLY", "1");
        // Never the real keyring.
        qputenv("ARGONAUT_KEYSTORE", "file");
        // Never the extensions installed on this computer (a test names its own).
        if (!qEnvironmentVariableIsSet("ARGONAUT_EXTENSIONS_PATH"))
            qputenv("ARGONAUT_EXTENSIONS_PATH", "");
        if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
            qputenv("QT_QPA_PLATFORM", "offscreen");
    }
    bool isValid() const { return m_dir.isValid(); }
    QString path() const { return m_dir.path(); }

private:
    QTemporaryDir m_dir;
};

// What a finished file job did.
struct Outcome
{
    FileJob::Kind kind = FileJob::Kind::Copy;
    QList<TransferResult> results;
    QStringList errors;
    bool canceled = false;
    bool userAction = true;
    bool timedOut = false;
};

// Runs the event loop until the next job of `ops` finishes. A job that never
// finishes fails the test and leaves an error in the outcome: it must not
// look like a job that ended without errors.
inline Outcome waitForJob(FileOperations *ops, int timeoutMs = 120000)
{
    Outcome outcome;
    bool done = false;
    QEventLoop loop;
    const auto connection = QObject::connect(ops, &FileOperations::jobFinished, &loop, [&](FileJob *job) {
        outcome.kind = job->kind();
        outcome.results = job->results();
        outcome.errors = job->errors();
        outcome.canceled = job->isCanceled();
        outcome.userAction = job->property(FileOperations::UserActionProperty).toBool();
        done = true;
        loop.quit();
    });
    QTimer::singleShot(timeoutMs, &loop, &QEventLoop::quit);
    loop.exec();
    QObject::disconnect(connection);
    if (!done) {
        outcome.timedOut = true;
        outcome.errors.append(QStringLiteral("timed out waiting for a file job"));
        QTest::qFail("timed out waiting for a file job", __FILE__, __LINE__);
    }
    return outcome;
}

} // namespace TestSupport
