#pragma once

// What the extensions that draw their own result share (System Dashboard,
// Space Overview): sending a message to Argonaut, the colours and font of a
// picture (Argonaut's theme, as its "result" message tells them), and the
// colour codes of the same thing drawn with characters for the terminal
// interface.

#include <QColor>
#include <QFont>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QSaveFile>
#include <QString>

#include <cstdio>

inline void send(const QJsonObject &message)
{
    const QByteArray line = QJsonDocument(message).toJson(QJsonDocument::Compact) + '\n';
    std::fwrite(line.constData(), 1, size_t(line.size()), stdout);
    std::fflush(stdout);
}

// ------------------------------------------------------------ picture ---

// The colours and font of the picture: Argonaut's theme (so the result looks
// like part of the window, in every theme), or a dark or light look of its
// own until Argonaut has told them.
struct Look
{
    QColor window, card, track, text, dim, accent;
    QString font;
};

inline QColor mix(const QColor &a, const QColor &b, double share)
{
    return QColor::fromRgbF(a.redF() + (b.redF() - a.redF()) * share, a.greenF() + (b.greenF() - a.greenF()) * share,
                            a.blueF() + (b.blueF() - a.blueF()) * share);
}

inline Look lookFor(bool dark, const QJsonObject &colors = {}, const QString &font = {})
{
    Look look = dark ? Look{QColor(0x1e, 0x1e, 0x1e), QColor(0x2a, 0x2a, 0x2a), {}, QColor(0xf0, 0xf0, 0xf0), QColor(0x9a, 0x9a, 0x9a), QColor(0x35, 0x84, 0xe4), {}}
                     : Look{QColor(0xfa, 0xfa, 0xfa), QColor(0xeb, 0xeb, 0xeb), {}, QColor(0x24, 0x24, 0x24), QColor(0x6a, 0x6a, 0x6a), QColor(0x1c, 0x71, 0xd8), {}};
    auto take = [&colors](const char *name, QColor *into) {
        const QColor color(colors.value(QLatin1String(name)).toString());
        if (color.isValid())
            *into = color;
    };
    take("window", &look.window);
    take("surface", &look.card);
    take("text", &look.text);
    take("dim", &look.dim);
    take("accent", &look.accent);
    // A theme whose panels are the background's colour still gets panels.
    if (look.card == look.window)
        look.card = mix(look.window, look.text, 0.06);
    look.track = mix(look.card, look.text, 0.10); // the empty part of a gauge or bar
    look.font = font;
    return look;
}

// A gauge or bar is in the theme's accent colour, and warns when it is nearly full.
inline QColor levelColor(const Look &look, double share)
{
    if (share >= 0.9)
        return QColor(0xe0, 0x1b, 0x24);
    if (share >= 0.75)
        return QColor(0xe6, 0x61, 0x00);
    return look.accent;
}

inline QString g_font; // the family every text is drawn in (empty: Qt's own)

inline QFont fontOf(double pixels, bool bold = false)
{
    QFont font;
    if (!g_font.isEmpty())
        font.setFamily(g_font);
    font.setPixelSize(qMax(8, int(pixels + 0.5)));
    font.setBold(bold);
    return font;
}

// A panel; `u` is the size of ordinary text, the unit everything is laid out in.
inline void card(QPainter &p, const QRectF &r, const Look &look, double u)
{
    p.setPen(Qt::NoPen);
    p.setBrush(look.card);
    p.drawRoundedRect(r, u * 0.5, u * 0.5);
}

inline bool save(const QImage &image, const QString &path)
{
    // Whole or not at all: Argonaut reads it as soon as it is told.
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    return image.save(&file, "PNG", 80) && file.commit(); // 80: little compression, quick
}

// ---------------------------------------------- the same, in a terminal ---

inline QString sgr(int fg, int bg = -1)
{
    QString code = QStringLiteral("\x1b[0");
    if (fg >= 0)
        code += QStringLiteral(";38;5;%1").arg(fg);
    if (bg >= 0)
        code += QStringLiteral(";48;5;%1").arg(bg);
    return code + QLatin1Char('m');
}

inline const QString kReset = QStringLiteral("\x1b[0m");
inline const QString kBold = QStringLiteral("\x1b[0;1m");
inline const QString kDim = QStringLiteral("\x1b[0;38;5;8m");
constexpr int kTrack = 238;  // the empty part of a gauge or bar: a dark grey
constexpr int kAccent = 6;   // the terminal's own cyan, as the rest of the interface
constexpr int kNearlyFull = 208;
constexpr int kFull = 196;

inline int levelIndex(double share)
{
    return share >= 0.9 ? kFull : share >= 0.75 ? kNearlyFull : kAccent;
}
