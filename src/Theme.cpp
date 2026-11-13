#include "Theme.h"

#include <QApplication>
#include <QIconEngine>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QSettings>

#include <initializer_list>

namespace {

constexpr auto ThemeKey = "appearance/theme";

// The platform's own palette, captured the first time we touch the
// palette (before we ever replace it).
const QPalette &systemPalette()
{
    static const QPalette palette = QApplication::palette();
    return palette;
}

ThemeMode s_mode = ThemeMode::System; // what applyThemeMode() last applied

struct ThemeInfo
{
    ThemeMode mode;
    const char *key;  // saved in the settings
    const char *name; // shown in Preferences (translated)
};

const ThemeInfo kThemes[] = {
    {ThemeMode::System, "system", QT_TRANSLATE_NOOP("Theme", "Follow System")},
    {ThemeMode::Light, "light", QT_TRANSLATE_NOOP("Theme", "Light")},
    {ThemeMode::Dark, "dark", QT_TRANSLATE_NOOP("Theme", "Dark")},
    {ThemeMode::MidnightBlue, "midnight", QT_TRANSLATE_NOOP("Theme", "Midnight Blue")},
    {ThemeMode::RoseDawn, "rose", QT_TRANSLATE_NOOP("Theme", "Rosé Dawn")},
    {ThemeMode::Forest, "forest", QT_TRANSLATE_NOOP("Theme", "Forest")},
    {ThemeMode::Sunset, "sunset", QT_TRANSLATE_NOOP("Theme", "Sunset")},
};

const ThemeInfo &infoFor(ThemeMode mode)
{
    for (const ThemeInfo &info : kThemes) {
        if (info.mode == mode)
            return info;
    }
    return kThemes[0];
}

QColor withAlpha(QColor c, int alpha)
{
    c.setAlpha(alpha);
    return c;
}

// `over` (possibly translucent) painted on top of opaque `under`.
QColor blend(const QColor &over, const QColor &under)
{
    const qreal a = over.alphaF();
    return QColor::fromRgbF(over.redF() * a + under.redF() * (1 - a),
                            over.greenF() * a + under.greenF() * (1 - a),
                            over.blueF() * a + under.blueF() * (1 - a));
}

// The GNOME-like defaults (exact libadwaita colours).
Theme adwaita(bool dark, const QColor &accent)
{
    Theme t;
    t.dark = dark;
    t.accent = accent;
    if (dark) {
        t.window = QColor("#242424");
        t.sidebar = QColor("#303030");
        t.outline = QColor("#3d3d3d");
        t.fg = QColor(255, 255, 255);
        t.dimFg = QColor(255, 255, 255, 140);
        t.hover = QColor(255, 255, 255, 18);
        t.pressed = QColor(255, 255, 255, 36);
        t.selected = QColor(255, 255, 255, 26);
        t.pathBg = QColor(255, 255, 255, 20);
        t.button = QColor(255, 255, 255, 26);
        t.buttonHover = QColor(255, 255, 255, 46);
        t.popup = QColor("#383838");
        t.border = QColor(0, 0, 0, 90);
        t.scroll = QColor(255, 255, 255, 60);
        t.scrollHover = QColor(255, 255, 255, 110);
    } else {
        t.window = QColor("#fafafa");
        t.sidebar = QColor("#ebebeb");
        t.outline = QColor("#c8c8c8");
        t.fg = QColor(0, 0, 0, 204);
        t.dimFg = QColor(0, 0, 0, 140);
        t.hover = QColor(0, 0, 0, 13);
        t.pressed = QColor(0, 0, 0, 30);
        t.selected = QColor(0, 0, 0, 26);
        t.pathBg = QColor(0, 0, 0, 15);
        t.button = QColor(0, 0, 0, 20);
        t.buttonHover = QColor(0, 0, 0, 36);
        t.popup = QColor("#ffffff");
        t.border = QColor(0, 0, 0, 30);
        t.scroll = QColor(0, 0, 0, 60);
        t.scrollHover = QColor(0, 0, 0, 110);
    }
    t.itemSelected = withAlpha(t.accent, dark ? 80 : 60);
    return t;
}

// A coloured theme from four base colours; every other token is derived
// the same way as in the defaults, so all themes behave alike.
Theme custom(bool dark, const QColor &window, const QColor &sidebar, const QColor &fg,
             const QColor &accent)
{
    Theme t;
    t.dark = dark;
    t.window = window;
    t.sidebar = sidebar;
    t.fg = fg;
    t.accent = accent;
    if (dark) {
        t.outline = sidebar.lighter(135);
        t.dimFg = withAlpha(fg, 150);
        t.hover = withAlpha(fg, 20);
        t.pressed = withAlpha(fg, 38);
        t.selected = withAlpha(fg, 28);
        t.pathBg = withAlpha(fg, 22);
        t.button = withAlpha(fg, 28);
        t.buttonHover = withAlpha(fg, 48);
        t.popup = sidebar.lighter(118);
        t.border = QColor(0, 0, 0, 100);
        t.scroll = withAlpha(fg, 64);
        t.scrollHover = withAlpha(fg, 115);
        t.itemSelected = withAlpha(accent, 85);
    } else {
        t.outline = sidebar.darker(118);
        t.dimFg = withAlpha(fg, 165);
        t.hover = withAlpha(fg, 16);
        t.pressed = withAlpha(fg, 34);
        t.selected = withAlpha(fg, 30);
        t.pathBg = withAlpha(fg, 18);
        t.button = withAlpha(fg, 22);
        t.buttonHover = withAlpha(fg, 40);
        t.popup = window.lighter(103);
        t.border = withAlpha(fg, 40);
        t.scroll = withAlpha(fg, 64);
        t.scrollHover = withAlpha(fg, 115);
        t.itemSelected = withAlpha(accent, 64);
    }
    return t;
}

// Qt palette (for widgets our style sheet doesn't cover: text fields,
// dialogs, tooltips…) matching a theme.
QPalette paletteFor(const Theme &t)
{
    const QColor text = blend(t.fg, t.window);
    const QColor base = t.dark ? t.window.darker(112) : t.window.lighter(104);
    const QColor disabled = blend(withAlpha(t.fg, 110), t.window);

    QPalette p;
    p.setColor(QPalette::Window, t.window);
    p.setColor(QPalette::WindowText, text);
    p.setColor(QPalette::Base, base);
    p.setColor(QPalette::AlternateBase, blend(t.hover, base));
    p.setColor(QPalette::Text, text);
    p.setColor(QPalette::Button, t.sidebar);
    p.setColor(QPalette::ButtonText, text);
    p.setColor(QPalette::BrightText, t.dark ? QColor("#ff7b63") : QColor("#c01c28"));
    p.setColor(QPalette::ToolTipBase, t.popup);
    p.setColor(QPalette::ToolTipText, text);
    p.setColor(QPalette::PlaceholderText, blend(t.dimFg, base));
    p.setColor(QPalette::Highlight, t.accent);
    p.setColor(QPalette::HighlightedText, QColor("#ffffff"));
    p.setColor(QPalette::Link, t.accent);
    // Fusion's bevel shades
    p.setColor(QPalette::Light, t.sidebar.lighter(130));
    p.setColor(QPalette::Midlight, t.sidebar.lighter(115));
    p.setColor(QPalette::Mid, t.sidebar.darker(130));
    p.setColor(QPalette::Dark, t.sidebar.darker(160));
    p.setColor(QPalette::Shadow, QColor(0, 0, 0, t.dark ? 160 : 90));
    for (QPalette::ColorRole role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText})
        p.setColor(QPalette::Disabled, role, disabled);
    return p;
}

} // namespace

QList<ThemeMode> allThemeModes()
{
    QList<ThemeMode> modes;
    for (const ThemeInfo &info : kThemes)
        modes.append(info.mode);
    return modes;
}

QString themeModeName(ThemeMode mode)
{
    return QCoreApplication::translate("Theme", infoFor(mode).name);
}

ThemeMode savedThemeMode()
{
    const QString value = QSettings().value(ThemeKey, "system").toString();
    for (const ThemeInfo &info : kThemes) {
        if (value == QLatin1String(info.key))
            return info.mode;
    }
    return ThemeMode::System;
}

void applyThemeMode(ThemeMode mode)
{
    const QPalette &system = systemPalette();
    s_mode = mode;
    if (mode == ThemeMode::System) {
        // Leave the palette untouched at startup so Qt keeps following the
        // platform; only restore it if we replaced it earlier.
        if (QApplication::palette() != system)
            QApplication::setPalette(system);
        return;
    }
    QApplication::setPalette(paletteFor(Theme::forMode(mode)));
}

void setThemeMode(ThemeMode mode)
{
    QSettings().setValue(ThemeKey, QString::fromLatin1(infoFor(mode).key));
    applyThemeMode(mode);
}

Theme currentTheme()
{
    return s_mode == ThemeMode::System ? Theme::fromPalette(QApplication::palette())
                                       : Theme::forMode(s_mode);
}

Theme Theme::fromPalette(const QPalette &palette)
{
    const bool dark = palette.color(QPalette::Window).lightness() < 128;
    Theme t = adwaita(dark, palette.color(QPalette::Highlight));
    t.id = dark ? QStringLiteral("system-dark") : QStringLiteral("system-light");
    return t;
}

Theme Theme::forMode(ThemeMode mode)
{
    const QColor systemAccent = systemPalette().color(QPalette::Highlight);
    Theme t;
    switch (mode) {
    case ThemeMode::System:
        return fromPalette(systemPalette());
    case ThemeMode::Light:
        t = adwaita(false, systemAccent);
        break;
    case ThemeMode::Dark:
        t = adwaita(true, systemAccent);
        break;
    case ThemeMode::MidnightBlue: // deep navy, sky-blue accent
        t = custom(true, QColor("#141b26"), QColor("#1b2433"), QColor("#e6edf7"),
                   QColor("#5aa9ff"));
        break;
    case ThemeMode::RoseDawn: // warm cream, blush sidebar, rose accent
        t = custom(false, QColor("#fbf6f3"), QColor("#f3e6e4"), QColor("#4a3536"),
                   QColor("#d46a7e"));
        break;
    case ThemeMode::Forest: // deep green-grey, mint accent
        t = custom(true, QColor("#18201b"), QColor("#1f2a24"), QColor("#e3ece5"),
                   QColor("#5fcf8f"));
        break;
    case ThemeMode::Sunset: // plum and charcoal, amber accent
        t = custom(true, QColor("#201826"), QColor("#2a2030"), QColor("#f2e8f0"),
                   QColor("#ff9e5e"));
        break;
    }
    t.id = QString::fromLatin1(infoFor(mode).key);
    return t;
}

QString cssColor(const QColor &c)
{
    return QStringLiteral("rgba(%1, %2, %3, %4)")
        .arg(c.red())
        .arg(c.green())
        .arg(c.blue())
        .arg(c.alphaF(), 0, 'f', 3);
}

namespace {

// All glyphs are designed on a 16x16 grid with a 1.5px stroke.
void paintGlyph(QPainter *p, const QRectF &r, Glyph g, const QColor &c)
{
    p->save();
    p->setRenderHint(QPainter::Antialiasing);
    const qreal s = qMin(r.width(), r.height()) / 16.0;
    p->translate(r.center().x() - 8 * s, r.center().y() - 8 * s);
    p->scale(s, s);

    const QPen pen(c, 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    p->setPen(pen);
    p->setBrush(Qt::NoBrush);

    auto line = [&](qreal x1, qreal y1, qreal x2, qreal y2) {
        p->drawLine(QPointF(x1, y1), QPointF(x2, y2));
    };
    auto poly = [&](std::initializer_list<QPointF> pts, bool closed = false) {
        QPainterPath path;
        bool first = true;
        for (const QPointF &pt : pts) {
            if (first)
                path.moveTo(pt);
            else
                path.lineTo(pt);
            first = false;
        }
        if (closed)
            path.closeSubpath();
        p->drawPath(path);
    };
    auto filled = [&](auto draw) {
        p->setPen(Qt::NoPen);
        p->setBrush(c);
        draw();
        p->setBrush(Qt::NoBrush);
        p->setPen(pen);
    };

    switch (g) {
    case Glyph::Home:
        poly({{2, 8.25}, {8, 2.5}, {14, 8.25}});
        poly({{3.75, 6.75}, {3.75, 13.5}, {12.25, 13.5}, {12.25, 6.75}});
        poly({{6.75, 13.5}, {6.75, 10}, {9.25, 10}, {9.25, 13.5}});
        break;
    case Glyph::Desktop:
        p->drawRoundedRect(QRectF(1.75, 2.5, 12.5, 8.5), 1.5, 1.5);
        line(8, 11, 8, 13.5);
        line(5, 13.5, 11, 13.5);
        break;
    case Glyph::Documents:
        poly({{3.5, 1.75}, {9.25, 1.75}, {12.5, 5}, {12.5, 14.25}, {3.5, 14.25}}, true);
        poly({{9.25, 1.75}, {9.25, 5}, {12.5, 5}});
        line(5.75, 8, 10.25, 8);
        line(5.75, 10.75, 10.25, 10.75);
        break;
    case Glyph::Downloads:
        line(8, 2, 8, 10);
        poly({{4.5, 6.75}, {8, 10.25}, {11.5, 6.75}});
        poly({{2.5, 10.75}, {2.5, 13.5}, {13.5, 13.5}, {13.5, 10.75}});
        break;
    case Glyph::Music:
        poly({{6, 12}, {6, 3.25}, {13, 1.75}, {13, 10.5}});
        filled([&] {
            p->drawEllipse(QPointF(4.25, 12), 2.1, 1.7);
            p->drawEllipse(QPointF(11.25, 10.5), 2.1, 1.7);
        });
        break;
    case Glyph::Pictures:
        p->drawRoundedRect(QRectF(1.75, 2.75, 12.5, 10.5), 1.5, 1.5);
        poly({{2.25, 12}, {6, 8.25}, {9, 11.25}, {11, 9.25}, {13.75, 12}});
        filled([&] { p->drawEllipse(QPointF(10.75, 6), 1.3, 1.3); });
        break;
    case Glyph::Videos:
        p->drawRoundedRect(QRectF(1.75, 3, 12.5, 10), 1.5, 1.5);
        filled([&] {
            QPainterPath tri;
            tri.moveTo(6.5, 5.75);
            tri.lineTo(10.75, 8);
            tri.lineTo(6.5, 10.25);
            tri.closeSubpath();
            p->drawPath(tri);
        });
        break;
    case Glyph::Trash:
        line(2.5, 4, 13.5, 4);
        poly({{6, 4}, {6, 2.25}, {10, 2.25}, {10, 4}});
        poly({{3.75, 4}, {4.5, 14}, {11.5, 14}, {12.25, 4}});
        line(6.75, 6.75, 6.75, 11.25);
        line(9.25, 6.75, 9.25, 11.25);
        break;
    case Glyph::Drive:
        p->drawRoundedRect(QRectF(1.75, 4.5, 12.5, 7), 1.5, 1.5);
        line(4.25, 8, 7.75, 8);
        filled([&] { p->drawEllipse(QPointF(11.25, 8), 0.95, 0.95); });
        break;
    case Glyph::Removable:
        p->drawRoundedRect(QRectF(4.25, 5.5, 7.5, 8.75), 1.5, 1.5);
        p->drawRect(QRectF(6, 1.75, 4, 3.75));
        break;
    case Glyph::Search:
        p->drawEllipse(QPointF(7, 7), 4.25, 4.25);
        line(10.25, 10.25, 13.5, 13.5);
        break;
    case Glyph::Menu:
        for (qreal y : {4.0, 8.0, 12.0})
            line(2.75, y, 13.25, y);
        break;
    case Glyph::Kebab:
        filled([&] {
            for (qreal y : {3.5, 8.0, 12.5})
                p->drawEllipse(QPointF(8, y), 1.4, 1.4);
        });
        break;
    case Glyph::ChevronLeft:
        poly({{10, 3.5}, {5.5, 8}, {10, 12.5}});
        break;
    case Glyph::ChevronRight:
        poly({{6, 3.5}, {10.5, 8}, {6, 12.5}});
        break;
    case Glyph::Grid:
        for (const QRectF &rr : {QRectF(2.5, 2.5, 4.5, 4.5), QRectF(9, 2.5, 4.5, 4.5),
                                 QRectF(2.5, 9, 4.5, 4.5), QRectF(9, 9, 4.5, 4.5)})
            p->drawRoundedRect(rr, 1, 1);
        break;
    case Glyph::List:
        for (qreal y : {4.0, 8.0, 12.0})
            line(6.5, y, 13.5, y);
        filled([&] {
            for (qreal y : {4.0, 8.0, 12.0})
                p->drawEllipse(QPointF(3, y), 1.1, 1.1);
        });
        break;
    case Glyph::Minimize:
        line(4.5, 11, 11.5, 11);
        break;
    case Glyph::Maximize:
        p->drawRoundedRect(QRectF(4.25, 4.25, 7.5, 7.5), 1.2, 1.2);
        break;
    case Glyph::Restore:
        p->drawRoundedRect(QRectF(3.75, 6.25, 6, 6), 1, 1);
        poly({{6.5, 3.75}, {12.25, 3.75}, {12.25, 9.5}});
        break;
    case Glyph::Close:
        line(4.75, 4.75, 11.25, 11.25);
        line(11.25, 4.75, 4.75, 11.25);
        break;
    }
    p->restore();
}

class GlyphEngine : public QIconEngine
{
public:
    GlyphEngine(Glyph glyph, const QColor &color) : m_glyph(glyph), m_color(color) {}

    void paint(QPainter *painter, const QRect &rect, QIcon::Mode mode, QIcon::State) override
    {
        QColor c = m_color;
        if (mode == QIcon::Disabled)
            c.setAlphaF(c.alphaF() * 0.35);
        paintGlyph(painter, rect, m_glyph, c);
    }

    QPixmap pixmap(const QSize &size, QIcon::Mode mode, QIcon::State state) override
    {
        QPixmap pm(size);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        paint(&p, QRect(QPoint(0, 0), size), mode, state);
        return pm;
    }

    QIconEngine *clone() const override { return new GlyphEngine(m_glyph, m_color); }

private:
    Glyph m_glyph;
    QColor m_color;
};

} // namespace

QIcon glyphIcon(Glyph glyph, const QColor &color)
{
    return QIcon(new GlyphEngine(glyph, color));
}
