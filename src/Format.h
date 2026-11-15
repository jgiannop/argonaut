#pragma once

#include <QCoreApplication>
#include <QDir>
#include <QLocale>
#include <QString>

// File sizes the way GNOME (Nautilus, Disks, Settings) shows them: powers of
// 1000 with kB / MB / GB, so the numbers match the rest of the desktop and
// what's printed on drives. Use this everywhere instead of
// QLocale::formattedDataSize's default (KiB / MiB, powers of 1024).
inline QString formatSize(qint64 bytes)
{
    if (bytes < 1000) {
        return bytes == 1 ? QCoreApplication::translate("Format", "1 byte")
                          : QCoreApplication::translate("Format", "%1 bytes").arg(bytes);
    }
    return QLocale().formattedDataSize(bytes, 1, QLocale::DataSizeSIFormat);
}

// A folder for display, with ~ for the home folder (like Nautilus):
// "/home/ann/Music" -> "~/Music".
inline QString tildePath(const QString &path)
{
    const QString home = QDir::homePath();
    if (path == home || path.startsWith(home + QLatin1Char('/')))
        return QDir::toNativeSeparators(QLatin1Char('~') + path.mid(home.size()));
    return QDir::toNativeSeparators(path);
}
