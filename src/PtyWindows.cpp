#include "Pty.h"

#include <QDir>
#include <QFileInfo>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QThread>
#include <QWinEventNotifier>

#include <windows.h>
#include <tlhelp32.h>

// ConPTY: Windows' own pseudo-terminal (Windows 10 1809 and later). The shell
// runs attached to a pseudo console; we write keys (VT sequences, UTF-8)
// into one pipe and read the screen's stream (VT sequences, UTF-8) from the
// other, which ConPTY makes for us. The functions are looked up at run time,
// so the program still starts on a Windows without them (no terminal there).

namespace {

using PseudoConsole = void *; // HPCON
using CreateFn = HRESULT(WINAPI *)(COORD, HANDLE, HANDLE, DWORD, PseudoConsole *);
using ResizeFn = HRESULT(WINAPI *)(PseudoConsole, COORD);
using CloseFn = void(WINAPI *)(PseudoConsole);

constexpr DWORD_PTR AttributePseudoConsole = 0x00020016; // PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE

struct ConPty {
    CreateFn create = nullptr;
    ResizeFn resize = nullptr;
    CloseFn close = nullptr;
};

const ConPty &conPty()
{
    static const ConPty api = [] {
        ConPty a;
        if (HMODULE kernel = ::GetModuleHandleW(L"kernel32.dll")) {
            a.create = reinterpret_cast<CreateFn>(::GetProcAddress(kernel, "CreatePseudoConsole"));
            a.resize = reinterpret_cast<ResizeFn>(::GetProcAddress(kernel, "ResizePseudoConsole"));
            a.close = reinterpret_cast<CloseFn>(::GetProcAddress(kernel, "ClosePseudoConsole"));
        }
        return a;
    }();
    return api;
}

COORD sizeOf(int rows, int cols)
{
    return COORD{SHORT(qBound(1, cols, 32767)), SHORT(qBound(1, rows, 32767))};
}

enum class Kind { PowerShell, Cmd, Other };

Kind kindOf(const QString &program)
{
    const QString name = QFileInfo(program).baseName().toLower();
    if (name == QLatin1String("pwsh") || name == QLatin1String("powershell"))
        return Kind::PowerShell;
    if (name == QLatin1String("cmd"))
        return Kind::Cmd;
    return Kind::Other;
}

// $SHELL when it names a program (Git Bash, MSYS2 set it), else PowerShell 7,
// else Windows PowerShell, else cmd.
QString shellProgram()
{
    const QString shell = qEnvironmentVariable("SHELL");
    if (!shell.isEmpty() && QFileInfo(shell).isFile())
        return QDir::toNativeSeparators(shell);
    for (const char *name : {"pwsh", "powershell"}) {
        const QString found = QStandardPaths::findExecutable(QString::fromLatin1(name));
        if (!found.isEmpty())
            return QDir::toNativeSeparators(found);
    }
    const QString comspec = qEnvironmentVariable("COMSPEC");
    return comspec.isEmpty() ? QStringLiteral("cmd.exe") : comspec;
}

// The prompt says where the shell is: OSC 9;9 (what Windows Terminal reads)
// and the window title, which ConPTY always passes on. TerminalWidget reads
// either, since Windows can't tell another process's folder (PowerShell's
// location isn't even its process's folder).
QString powerShellSetup()
{
    return QStringLiteral(
        "$global:__argonautPrompt = $function:prompt\n"
        "function global:prompt {\n"
        "  $where = $executionContext.SessionState.Path.CurrentFileSystemLocation.ProviderPath\n"
        "  try { $Host.UI.RawUI.WindowTitle = $where } catch {}\n"
        "  $e = [char]27\n"
        "  \"$e]9;9;$where$e\\\" + (& $global:__argonautPrompt)\n"
        "}\n");
}

QString quoteArgument(const QString &argument)
{
    if (!argument.isEmpty() && !argument.contains(QLatin1Char(' ')) && !argument.contains(QLatin1Char('"')))
        return argument;
    QString quoted = argument;
    quoted.replace(QLatin1String("\""), QLatin1String("\\\""));
    return QLatin1Char('"') + quoted + QLatin1Char('"');
}

// Reads what the pseudo console writes until the pipe closes (the console
// was closed). Blocking reads, so on a thread of its own.
class Reader : public QThread
{
    Q_OBJECT
public:
    explicit Reader(HANDLE pipe)
        : m_pipe(pipe)
    {
    }

signals:
    void received(const QByteArray &bytes);

protected:
    void run() override
    {
        char buffer[65536];
        DWORD n = 0;
        while (::ReadFile(m_pipe, buffer, sizeof buffer, &n, nullptr) && n > 0)
            emit received(QByteArray(buffer, qsizetype(n)));
    }

private:
    HANDLE m_pipe;
};

} // namespace

struct Pty::Private {
    PseudoConsole console = nullptr;
    HANDLE input = INVALID_HANDLE_VALUE;  // we write keys here
    HANDLE output = INVALID_HANDLE_VALUE; // we read the screen here
    PROCESS_INFORMATION process{};
    Kind kind = Kind::Other;
    Reader *reader = nullptr;
    QWinEventNotifier *exitNotifier = nullptr;
    bool running = false;

    void closeConsole()
    {
        if (console) {
            conPty().close(console); // the pipe then closes, and the reader ends
            console = nullptr;
        }
    }
};

Pty::Pty(QObject *parent)
    : QObject(parent)
    , d(new Private)
{
}

Pty::~Pty()
{
    if (d->exitNotifier)
        d->exitNotifier->setEnabled(false);
    d->closeConsole(); // closes the shell's console: it is told to end
    if (d->process.hProcess) {
        if (::WaitForSingleObject(d->process.hProcess, 300) == WAIT_TIMEOUT)
            ::TerminateProcess(d->process.hProcess, 1);
        ::CloseHandle(d->process.hProcess);
        ::CloseHandle(d->process.hThread);
    }
    if (d->input != INVALID_HANDLE_VALUE)
        ::CloseHandle(d->input);
    if (d->reader) {
        d->reader->disconnect(this);
        if (!d->reader->wait(2000))
            d->reader->terminate();
        delete d->reader;
    }
    if (d->output != INVALID_HANDLE_VALUE)
        ::CloseHandle(d->output);
}

bool Pty::start(const QString &directory, int rows, int cols)
{
    if (d->running)
        return true;
    const ConPty &api = conPty();
    if (!api.create || !api.resize || !api.close)
        return false; // older than Windows 10 1809

    HANDLE keysRead = INVALID_HANDLE_VALUE, keysWrite = INVALID_HANDLE_VALUE;
    HANDLE screenRead = INVALID_HANDLE_VALUE, screenWrite = INVALID_HANDLE_VALUE;
    if (!::CreatePipe(&keysRead, &keysWrite, nullptr, 0))
        return false;
    if (!::CreatePipe(&screenRead, &screenWrite, nullptr, 0)) {
        ::CloseHandle(keysRead);
        ::CloseHandle(keysWrite);
        return false;
    }
    PseudoConsole console = nullptr;
    const HRESULT made = api.create(sizeOf(rows, cols), keysRead, screenWrite, 0, &console);
    // The console has its own copies of its ends; ours would keep the pipes
    // open after it has gone.
    ::CloseHandle(keysRead);
    ::CloseHandle(screenWrite);
    if (FAILED(made)) {
        ::CloseHandle(keysWrite);
        ::CloseHandle(screenRead);
        return false;
    }

    const QString program = shellProgram();
    d->kind = kindOf(program);
    QString commandLine = quoteArgument(program);
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("TERM"), QStringLiteral("xterm-256color"));
    env.insert(QStringLiteral("COLORTERM"), QStringLiteral("truecolor"));
    env.insert(QStringLiteral("TERM_PROGRAM"), QStringLiteral("argonaut"));
    if (d->kind == Kind::PowerShell) {
        const QString script = powerShellSetup();
        const QByteArray utf16(reinterpret_cast<const char *>(script.utf16()), script.size() * 2);
        commandLine += QStringLiteral(" -NoLogo -NoExit -EncodedCommand ") + QString::fromLatin1(utf16.toBase64());
    } else if (d->kind == Kind::Cmd) {
        // $E]9;9;$P$E\ says the folder; $E]2;…$E\ makes it the title too.
        const QString own = env.value(QStringLiteral("PROMPT"), QStringLiteral("$P$G"));
        env.insert(QStringLiteral("PROMPT"), QStringLiteral("$E]9;9;$P$E\\$E]2;$P$E\\") + own);
    }
    QString block;
    for (const QString &entry : env.toStringList())
        block += entry + QChar(u'\0');
    block += QChar(u'\0');

    SIZE_T attributeSize = 0;
    ::InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeSize);
    QByteArray attributes(qsizetype(attributeSize), '\0');
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof startup;
    startup.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
    bool ok = ::InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &attributeSize)
        && ::UpdateProcThreadAttribute(startup.lpAttributeList, 0, AttributePseudoConsole, console, sizeof console, nullptr, nullptr);

    std::wstring command = commandLine.toStdWString();
    const std::wstring folder = QDir::toNativeSeparators(directory).toStdWString();
    const DWORD folderAttributes = ::GetFileAttributesW(folder.c_str());
    const bool folderExists = folderAttributes != INVALID_FILE_ATTRIBUTES && (folderAttributes & FILE_ATTRIBUTE_DIRECTORY);
    PROCESS_INFORMATION process{};
    ok = ok
        && ::CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE,
                            EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT,
                            const_cast<wchar_t *>(reinterpret_cast<const wchar_t *>(block.utf16())),
                            folderExists ? folder.c_str() : nullptr, &startup.StartupInfo, &process);
    ::DeleteProcThreadAttributeList(startup.lpAttributeList);
    if (!ok) {
        api.close(console);
        ::CloseHandle(keysWrite);
        ::CloseHandle(screenRead);
        return false;
    }

    d->console = console;
    d->input = keysWrite;
    d->output = screenRead;
    d->process = process;
    d->running = true;

    d->reader = new Reader(screenRead);
    connect(d->reader, &Reader::received, this, &Pty::received, Qt::QueuedConnection);
    // The pipe closes only once the console is closed, which happens when
    // the shell has ended: then everything it wrote has been read.
    connect(d->reader, &QThread::finished, this, [this] {
        if (!d->running)
            return;
        d->running = false;
        emit finished();
    });
    d->reader->start();

    d->exitNotifier = new QWinEventNotifier(process.hProcess, this);
    connect(d->exitNotifier, &QWinEventNotifier::activated, this, [this] {
        d->exitNotifier->setEnabled(false);
        d->closeConsole();
    });
    return true;
}

bool Pty::isRunning() const
{
    return d->running;
}

void Pty::write(const QByteArray &bytes)
{
    if (!d->running || d->input == INVALID_HANDLE_VALUE || bytes.isEmpty())
        return;
    qsizetype done = 0;
    while (done < bytes.size()) {
        DWORD n = 0;
        if (!::WriteFile(d->input, bytes.constData() + done, DWORD(bytes.size() - done), &n, nullptr) || n == 0)
            break;
        done += qsizetype(n);
    }
}

void Pty::resize(int rows, int cols)
{
    if (d->console)
        conPty().resize(d->console, sizeOf(rows, cols));
}

bool Pty::isIdle() const
{
    if (!d->running || !d->process.dwProcessId)
        return false;
    // Idle: the shell has started no program that is still running.
    HANDLE snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return false;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof entry;
    bool child = false;
    for (bool more = ::Process32FirstW(snapshot, &entry); more && !child; more = ::Process32NextW(snapshot, &entry))
        child = entry.th32ParentProcessID == d->process.dwProcessId;
    ::CloseHandle(snapshot);
    return !child;
}

QString Pty::currentDirectory() const
{
    return {}; // the shell says it (see powerShellSetup)
}

QByteArray Pty::changeDirectoryCommand(const QString &directory) const
{
    const QString native = QDir::toNativeSeparators(directory);
    switch (d->kind) {
    case Kind::PowerShell: {
        QString quoted = native;
        quoted.replace(QLatin1String("'"), QLatin1String("''"));
        // Ctrl+End, Ctrl+Home: PSReadLine deletes what is typed after and
        // before the cursor, so the line holds only the command.
        return "\x1b[1;5F\x1b[1;5H Set-Location -LiteralPath '" + quoted.toUtf8() + "'\r";
    }
    case Kind::Cmd:
        return "cd /d \"" + native.toUtf8() + "\"\r";
    case Kind::Other: // a Unix shell for Windows (Git Bash, MSYS2)
        break;
    }
    QString quoted = QDir::fromNativeSeparators(directory);
    quoted.replace(QLatin1String("'"), QLatin1String("'\\''"));
    return "\x05\x15 cd -- '" + quoted.toUtf8() + "'\r";
}

#include "PtyWindows.moc"
