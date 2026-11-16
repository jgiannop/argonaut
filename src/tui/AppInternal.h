#pragma once

// What the files that make up Tui::App share among themselves (App.cpp,
// AppMenus.cpp, AppPreview.cpp): not part of the interface's own header.

#include "ContentType.h"
#include "Screen.h"

#include <QDateTime>
#include <QFileInfo>
#include <QRgb>

namespace Tui {

inline const Style kBar{Ink::Normal, Paper::Bar, false};
inline const Style kDim{Ink::Dim, Paper::Normal, false};

// A colour as one of the terminal's 256: the 6×6×6 cube, or a grey.
inline qint16 nearest256(QRgb c)
{
    const int r = qRed(c), g = qGreen(c), b = qBlue(c);
    if (qAbs(r - g) < 12 && qAbs(g - b) < 12) {
        if (r < 8)
            return 16;
        if (r > 238)
            return 231;
        return qint16(232 + (r - 8) / 10);
    }
    auto level = [](int v) { return v < 48 ? 0 : v < 115 ? 1 : (v - 35) / 40; };
    return qint16(16 + 36 * level(r) + 6 * level(g) + level(b));
}

// What the file really is (its content, not only its name), as the window
// decides it: the same check, the same cache.
inline ContentType::Result typeOf(const QString &path)
{
    const QFileInfo info(path);
    const qint64 mtime = info.lastModified().toMSecsSinceEpoch();
    ContentType::Result result;
    if (!ContentType::Cache::instance().lookup(path, info.size(), mtime, &result)) {
        result = ContentType::inspect(path);
        ContentType::Cache::instance().store(path, info.size(), mtime, result);
    }
    return result;
}

} // namespace Tui
