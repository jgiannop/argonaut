#pragma once

#include <QString>

struct Theme;

// The one style sheet for the whole app (windows, dialogs, menus), built
// from a theme's colours. Qt widgets are styled here and only here, so the
// look is the same on every platform; MainWindow::applyTheme sets it again
// whenever the theme changes.
QString styleSheetFor(const Theme &theme);
