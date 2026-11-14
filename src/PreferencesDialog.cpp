#include "PreferencesDialog.h"

#include "DefaultFileManager.h"
#include "MainWindow.h"
#include "Theme.h"
#include "Thumbnailer.h"

#include <QApplication>
#include <QButtonGroup>
#include <QCheckBox>
#include <QSettings>
#include <QComboBox>
#include <QPushButton>
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
    auto *hint = new QLabel(tr("Choose how Kosmos looks."), this);
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

#if defined(Q_OS_LINUX)
    // Making thumbnails for PDFs and videos reads the files: cheap on the
    // internal disk, slow over USB or the network.
    layout->addSpacing(18);
    auto *thumbsHeading = new QLabel(tr("Thumbnails"), this);
    thumbsHeading->setObjectName("prefHeading");
    auto *thumbsHint = new QLabel(tr("Previews for PDFs, videos, fonts and more, made with the "
                                     "thumbnailers installed on this computer and shared with "
                                     "other apps."),
                                  this);
    thumbsHint->setObjectName("prefHint");
    thumbsHint->setWordWrap(true);
    auto *thumbs = new QComboBox(this);
    thumbs->setObjectName("propCombo");
    thumbs->addItem(tr("Make previews on this computer's disks only"), int(Thumbnailer::Policy::LocalOnly));
    thumbs->addItem(tr("Make previews everywhere, also USB and network drives"), int(Thumbnailer::Policy::Everywhere));
    thumbs->addItem(tr("Only show previews that already exist"), int(Thumbnailer::Policy::Never));
    thumbs->setCurrentIndex(thumbs->findData(int(Thumbnailer::policy())));
    connect(thumbs, &QComboBox::currentIndexChanged, this, [thumbs] {
        Thumbnailer::setPolicy(Thumbnailer::Policy(thumbs->currentData().toInt()));
    });
    layout->addWidget(thumbsHeading);
    layout->addWidget(thumbsHint);
    layout->addSpacing(6);
    layout->addWidget(thumbs);

    // Expandable folders in the list view, like Nautilus.
    layout->addSpacing(18);
    auto *listHeading = new QLabel(tr("List View"), this);
    listHeading->setObjectName("prefHeading");
    auto *expand = new QCheckBox(tr("Expandable folders"), this);
    expand->setObjectName("prefCheck");
    expand->setChecked(QSettings().value("view/expandFolders", false).toBool());
    auto *expandHint = new QLabel(tr("Show an arrow before each folder in the list, to open it in place. "
                                     "→ and ← open and close the selected folder."),
                                  this);
    expandHint->setObjectName("prefHint");
    expandHint->setWordWrap(true);
    connect(expand, &QCheckBox::toggled, this, [](bool on) { MainWindow::setExpandFoldersEverywhere(on); });
    layout->addWidget(listHeading);
    layout->addSpacing(4);
    layout->addWidget(expand);
    layout->addWidget(expandHint);

    // Default file manager: Kosmos or Nautilus, switchable both ways.
    layout->addSpacing(18);
    auto *defaultHeading = new QLabel(tr("Default File Manager"), this);
    defaultHeading->setObjectName("prefHeading");
    auto *defaultHint = new QLabel(this);
    defaultHint->setObjectName("prefHint");
    defaultHint->setWordWrap(true);
    auto *defaultButton = new QPushButton(this);
    defaultButton->setObjectName("prefButton");
    defaultButton->setCursor(Qt::PointingHandCursor);
    auto *defaultRow = new QHBoxLayout;
    defaultRow->addWidget(defaultButton);
    defaultRow->addStretch();
    auto refresh = [defaultHint, defaultButton] {
        const bool installed = !DefaultFileManager::installedDesktopFile().isEmpty();
        const bool ours = installed && DefaultFileManager::isKosmosDefault();
        if (!installed) {
            defaultHint->setText(tr("Install Kosmos (cmake --install) to make it the file manager "
                                    "for folders and “Show in folder”."));
            defaultButton->setText(tr("Make Kosmos the Default"));
            defaultButton->setEnabled(false);
        } else if (ours) {
            defaultHint->setText(tr("Kosmos opens your folders, also from other apps, Super+E and "
                                    "“Show in folder”. You can always go back (or run "
                                    "“kosmos --restore-nautilus” in a terminal)."));
            defaultButton->setText(tr("Use Nautilus Again"));
            defaultButton->setEnabled(true);
        } else {
            defaultHint->setText(tr("Another file manager opens your folders now. Kosmos can take "
                                    "over folders, Super+E and “Show in folder”; Nautilus stays "
                                    "installed and you can switch back at any time."));
            defaultButton->setText(tr("Make Kosmos the Default"));
            defaultButton->setEnabled(true);
        }
    };
    refresh();
    connect(defaultButton, &QPushButton::clicked, this, [this, refresh, defaultHint] {
        QString error;
        const bool ok = DefaultFileManager::isKosmosDefault()
                            ? DefaultFileManager::restorePrevious(&error)
                            : DefaultFileManager::makeDefault(&error);
        refresh();
        if (!ok)
            defaultHint->setText(error);
        adjustSize();
    });
    layout->addWidget(defaultHeading);
    layout->addWidget(defaultHint);
    layout->addSpacing(6);
    layout->addLayout(defaultRow);
#endif

    setFixedWidth(sizeHint().width()); // height follows the text (it changes when switching)
}
