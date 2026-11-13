#pragma once

#include <QColor>
#include <QIcon>
#include <QPalette>
#include <QString>

// Colour tokens modelled on GNOME's libadwaita, picked light or dark from
// the application palette.
struct Theme
{
    bool dark = false;
    QColor window;      // content area and its header bar
    QColor sidebar;     // sidebar and its header bar
    QColor outline;     // 1px window edge (client-side decorations)
    QColor fg;
    QColor dimFg;
    QColor hover;       // translucent overlays on top of a background
    QColor pressed;
    QColor selected;    // sidebar selection
    QColor accent;
    QColor itemSelected; // file grid/list selection (accent tint)
    QColor pathBg;      // path bar / search entry pill
    QColor button;      // round window-control buttons
    QColor buttonHover;
    QColor popup;       // menus, floating bar
    QColor border;
    QColor scroll;
    QColor scrollHover;

    static Theme fromPalette(const QPalette &palette);
};

// Light/dark preference. "System" keeps the platform palette.
enum class ThemeMode { System, Light, Dark };

ThemeMode savedThemeMode();
// Apply `mode` to the whole application (sets QApplication's palette,
// which sends ApplicationPaletteChange to every widget).
void applyThemeMode(ThemeMode mode);
// Apply and remember for the next start.
void setThemeMode(ThemeMode mode);
// Tokens for a forced light or dark theme (for previews).
Theme themeFor(bool dark);

// "rgba(r, g, b, a)" for use in Qt style sheets.
QString cssColor(const QColor &color);

// Monochrome UI icons, drawn in code so they look the same on every
// platform and need no icon theme or SVG plugin.
enum class Glyph {
    Home, Desktop, Documents, Downloads, Music, Pictures, Videos, Trash,
    Drive, Removable,
    Search, Menu, Kebab, ChevronLeft, ChevronRight, Grid, List,
    Minimize, Maximize, Restore, Close,
};

QIcon glyphIcon(Glyph glyph, const QColor &color);
