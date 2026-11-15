#pragma once

#include <QtGlobal>

#if defined(Q_OS_UNIX)
#include <sys/stat.h>

// A file's modification time in milliseconds, straight from stat(): the
// field has another name on macOS. (Not QFileInfo::lastModified(): it
// converts to local time, which costs two system calls per file.)
inline qint64 statMtimeMs(const struct stat &st)
{
#if defined(Q_OS_MACOS)
    return qint64(st.st_mtimespec.tv_sec) * 1000 + st.st_mtimespec.tv_nsec / 1000000;
#else
    return qint64(st.st_mtim.tv_sec) * 1000 + st.st_mtim.tv_nsec / 1000000;
#endif
}
#endif
