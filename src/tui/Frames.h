#pragma once

#include <QString>

// The lines of a frame drawn with box characters (a menu, the command
// palette, the cursor of the big icons).
namespace Tui {

// The top and bottom lines of a frame `width` cells wide: rounded corners
// with the effects on, square ones for the plain look.
inline QString frameTop(int width, bool rounded)
{
    return (rounded ? QStringLiteral("╭") : QStringLiteral("┌")) + QString(qMax(0, width - 2), QChar(0x2500))
           + (rounded ? QStringLiteral("╮") : QStringLiteral("┐"));
}

inline QString frameBottom(int width, bool rounded)
{
    return (rounded ? QStringLiteral("╰") : QStringLiteral("└")) + QString(qMax(0, width - 2), QChar(0x2500))
           + (rounded ? QStringLiteral("╯") : QStringLiteral("┘"));
}

} // namespace Tui
