#include "PreferencesDialog.h"

#include "Theme.h"

#include <QApplication>
#include <QButtonGroup>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QToolButton>
#include <QVBoxLayout>

namespace {

constexpr QSize PreviewSize(112, 72);

// A tiny drawing of the main window in the given theme.
void drawMockWindow(QPainter &p, const Theme &t, const QPainterPath &clip)
{
    p.save();
    p.setClipPath(clip, Qt::IntersectClip);
    const QRectF r(0, 0, PreviewSize.width(), PreviewSize.height());
    p.fillRect(r, t.window);
    p.fillRect(QRectF(0, 0, 34, r.height()), t.sidebar);

    QColor bar = t.fg;
    bar.setAlpha(70);
    for (int i = 0; i < 4; ++i) {
        QPainterPath row;
        row.addRoundedRect(QRectF(6, 14 + i * 12, i == 0 ? 22 : 18, 5), 2.5, 2.5);
        p.fillPath(row, i == 0 ? t.fg : bar);
    }

    QPainterPath pill;
    pill.addRoundedRect(QRectF(42, 7, 52, 9), 4.5, 4.5);
    p.fillPath(pill, t.pathBg);

    for (int row = 0; row < 2; ++row) {
        for (int col = 0; col < 3; ++col) {
            QPainterPath tile;
            tile.addRoundedRect(QRectF(46 + col * 21, 26 + row * 22, 14, 11), 2.5, 2.5);
            p.fillPath(tile, t.accent);
            QPainterPath label;
            label.addRoundedRect(QRectF(46 + col * 21, 40 + row * 22, 14, 3), 1.5, 1.5);
            p.fillPath(label, bar);
        }
    }
    p.restore();
}

QIcon previewIcon(ThemeMode mode)
{
    const qreal dpr = qApp->devicePixelRatio();
    QPixmap pm(PreviewSize * dpr);
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);

    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    QPainterPath frame;
    frame.addRoundedRect(QRectF(0.5, 0.5, PreviewSize.width() - 1, PreviewSize.height() - 1), 8, 8);

    const Theme light = Theme::forMode(ThemeMode::Light);
    const Theme dark = Theme::forMode(ThemeMode::Dark);
    if (mode == ThemeMode::System) {
        // Split diagonally: light top-left, dark bottom-right.
        QPainterPath lightHalf;
        lightHalf.moveTo(0, 0);
        lightHalf.lineTo(PreviewSize.width(), 0);
        lightHalf.lineTo(0, PreviewSize.height());
        lightHalf.closeSubpath();
        QPainterPath darkHalf;
        darkHalf.addRect(QRectF(QPointF(0, 0), QSizeF(PreviewSize)));
        darkHalf = darkHalf.subtracted(lightHalf);
        drawMockWindow(p, light, frame.intersected(lightHalf));
        drawMockWindow(p, dark, frame.intersected(darkHalf));
    } else {
        drawMockWindow(p, Theme::forMode(mode), frame);
    }

    p.setPen(QPen(QColor(0, 0, 0, 50), 1));
    p.setBrush(Qt::NoBrush);
    p.drawPath(frame);
    p.end();
    return QIcon(pm);
}

} // namespace

PreferencesDialog::PreferencesDialog(QWidget *parent)
    : QDialog(parent)
{
    setObjectName("preferences");
    setWindowTitle(tr("Preferences"));

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(24, 20, 24, 24);
    layout->setSpacing(6);

    auto *heading = new QLabel(tr("Appearance"), this);
    heading->setObjectName("prefHeading");
    auto *hint = new QLabel(tr("Choose how Kaftilous looks."), this);
    hint->setObjectName("prefHint");
    layout->addWidget(heading);
    layout->addWidget(hint);
    layout->addSpacing(10);

    auto *cards = new QGridLayout;
    cards->setSpacing(10);
    auto *group = new QButtonGroup(this);
    group->setExclusive(true);

    const ThemeMode current = savedThemeMode();
    const QList<ThemeMode> modes = allThemeModes();
    constexpr int Columns = 4;
    for (int i = 0; i < modes.size(); ++i) {
        const ThemeMode mode = modes.at(i);
        auto *card = new QToolButton(this);
        card->setObjectName("themeCard");
        card->setCheckable(true);
        card->setChecked(mode == current);
        card->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
        card->setText(themeModeName(mode));
        card->setIcon(previewIcon(mode));
        card->setIconSize(PreviewSize);
        card->setCursor(Qt::PointingHandCursor);
        group->addButton(card);
        cards->addWidget(card, i / Columns, i % Columns);
        connect(card, &QToolButton::clicked, this, [mode] { setThemeMode(mode); });
    }
    layout->addLayout(cards);

    setFixedSize(sizeHint());
}
