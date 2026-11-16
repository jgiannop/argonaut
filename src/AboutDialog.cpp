#include "AboutDialog.h"
#include "Sandbox.h"

#include "DefaultFileManager.h"
#include "FileJob.h"
#include "Theme.h"

#include <QApplication>
#include <QButtonGroup>
#include <QClipboard>
#include <QDir>
#include <QGridLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSettings>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QSysInfo>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

namespace {

struct Detail
{
    QString label;
    QString value;
};

QString yesNo(bool on, const QString &yes, const QString &no)
{
    return on ? yes : no;
}

QList<Detail> details()
{
    QList<Detail> list;
    list.append({QObject::tr("Argonaut"), QApplication::applicationVersion()});
    list.append({QObject::tr("Qt"), QObject::tr("%1 (built with %2)").arg(QString::fromLatin1(qVersion()),
                                                                        QStringLiteral(QT_VERSION_STR))});
    list.append({QObject::tr("System"), QSysInfo::prettyProductName()});
    list.append({QObject::tr("Kernel"), QSysInfo::kernelType() + QLatin1Char(' ') + QSysInfo::kernelVersion()});
    list.append({QObject::tr("Architecture"), QSysInfo::currentCpuArchitecture()});
    QString desktop = qEnvironmentVariable("XDG_CURRENT_DESKTOP");
    desktop.replace(QLatin1Char(':'), QStringLiteral(", "));
    const QString platform = QGuiApplication::platformName();
    list.append({QObject::tr("Desktop"), desktop.isEmpty() ? platform : QStringLiteral("%1 (%2)").arg(desktop, platform)});

#if defined(ARGONAUT_LIBARCHIVE_VERSION)
    list.append({QObject::tr("Archives"), QObject::tr("Yes, libarchive %1").arg(QStringLiteral(ARGONAUT_LIBARCHIVE_VERSION))});
#else
    list.append({QObject::tr("Archives"), QObject::tr("Not in this build")});
#endif
#if defined(ARGONAUT_TERMINAL)
    list.append({QObject::tr("Terminal"), QObject::tr("Yes, libvterm %1").arg(QStringLiteral(ARGONAUT_VTERM_VERSION))});
#else
    list.append({QObject::tr("Terminal"), QObject::tr("Not in this build")});
#endif
#if defined(Q_OS_LINUX)
    // The box untrusted files are read in (thumbnails, previews, details).
    list.append({QObject::tr("Sandbox"),
                 !Sandbox::available() ? QObject::tr("No (install bubblewrap)")
                 : Sandbox::works() ? QObject::tr("Yes, bubblewrap")
                 : QObject::tr("bubblewrap is installed, but the system doesn't let this Argonaut use it "
                               "(AppArmor: install Argonaut from its package)")});
    list.append({QObject::tr("Default file manager"),
                 yesNo(DefaultFileManager::isArgonautDefault(), QObject::tr("Argonaut"), QObject::tr("Another app"))});
#endif
    list.append({QObject::tr("Settings"), QDir::toNativeSeparators(QSettings().fileName())});
    list.append({QObject::tr("Cache"),
                 QDir::toNativeSeparators(QStandardPaths::writableLocation(QStandardPaths::CacheLocation))});
    return list;
}

QLabel *paragraph(const QString &text, QWidget *parent, const char *objectName = "aboutText")
{
    auto *label = new QLabel(text, parent);
    label->setObjectName(objectName);
    label->setWordWrap(true);
    label->setTextFormat(Qt::RichText);
    label->setOpenExternalLinks(true);
    QPalette palette = label->palette();
    palette.setColor(QPalette::Link, currentTheme().accent); // not the default blue
    label->setPalette(palette);
    return label;
}

// A rounded card with a bold heading and a paragraph, as on the About page.
QWidget *card(const QString &heading, const QString &html, QWidget *parent)
{
    auto *box = new QWidget(parent);
    box->setObjectName("aboutCard");
    auto *layout = new QVBoxLayout(box);
    layout->setContentsMargins(14, 10, 14, 12);
    layout->setSpacing(3);
    auto *title = new QLabel(heading, box);
    title->setObjectName("aboutCardHeading");
    layout->addWidget(title);
    layout->addWidget(paragraph(html, box));
    return box;
}

} // namespace

AboutDialog::AboutDialog(QWidget *parent) : QDialog(parent)
{
    setObjectName("aboutDialog");
    setWindowTitle(tr("About Argonaut"));
    setAttribute(Qt::WA_DeleteOnClose);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(28, 26, 28, 20);
    layout->setSpacing(0);

    // Icon, name, version, tagline.
    auto *icon = new QLabel(this);
    icon->setPixmap(applicationIcon().pixmap(QSize(96, 96), devicePixelRatioF()));
    icon->setAlignment(Qt::AlignCenter);
    layout->addWidget(icon);
    layout->addSpacing(10);

    auto *name = new QLabel(QStringLiteral("Argonaut"), this);
    name->setObjectName("aboutName");
    name->setAlignment(Qt::AlignCenter);
    layout->addWidget(name);
    layout->addSpacing(6);

    auto *versionRow = new QHBoxLayout;
    versionRow->addStretch();
    auto *version = new QLabel(tr("Version %1").arg(QApplication::applicationVersion()), this);
    version->setObjectName("aboutVersion");
    versionRow->addWidget(version);
    versionRow->addStretch();
    layout->addLayout(versionRow);
    layout->addSpacing(10);

    auto *tagline = new QLabel(tr("A fast file manager, inspired by GNOME Files"), this);
    tagline->setObjectName("aboutTagline");
    tagline->setAlignment(Qt::AlignCenter);
    layout->addWidget(tagline);
    layout->addSpacing(18);

    // Page switcher: About · System · Credits.
    auto *tabs = new QHBoxLayout;
    tabs->setSpacing(4);
    tabs->addStretch();
    auto *group = new QButtonGroup(this);
    const QStringList names = {tr("About"), tr("System"), tr("Credits")};
    for (int i = 0; i < names.size(); ++i) {
        auto *tab = new QToolButton(this);
        tab->setObjectName("aboutTab");
        tab->setText(names[i]);
        tab->setCheckable(true);
        tab->setChecked(i == 0);
        tab->setCursor(Qt::PointingHandCursor);
        group->addButton(tab, i);
        tabs->addWidget(tab);
    }
    tabs->addStretch();
    layout->addLayout(tabs);
    layout->addSpacing(14);

    m_pages = new QStackedWidget(this);
    m_pages->addWidget(buildAboutPage());
    m_pages->addWidget(buildSystemPage());
    m_pages->addWidget(buildCreditsPage());
    layout->addWidget(m_pages, 1);
    connect(group, &QButtonGroup::idClicked, m_pages, &QStackedWidget::setCurrentIndex);

    layout->addSpacing(16);
    auto *buttons = new QHBoxLayout;
    auto *copy = new QPushButton(tr("Copy Details"), this);
    copy->setObjectName("prefButton");
    copy->setToolTip(tr("Copy the version and system details, for a bug report"));
    connect(copy, &QPushButton::clicked, this, [copy] {
        QApplication::clipboard()->setText(systemDetails());
        copy->setText(tr("Copied"));
        QTimer::singleShot(1500, copy, [copy] { copy->setText(tr("Copy Details")); });
    });
    buttons->addWidget(copy);
    buttons->addStretch();
    auto *close = new QPushButton(tr("Close"), this);
    close->setObjectName("aboutClose");
    close->setDefault(true);
    connect(close, &QPushButton::clicked, this, &QDialog::accept);
    buttons->addWidget(close);
    layout->addLayout(buttons);

    setFixedWidth(500);
}

QString AboutDialog::systemDetails()
{
    QStringList lines;
    for (const Detail &d : details())
        lines << d.label + QStringLiteral(": ") + d.value;
    return lines.join(QLatin1Char('\n'));
}

QWidget *AboutDialog::buildAboutPage()
{
    auto *page = new QWidget(m_pages);
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(12);
    layout->addWidget(paragraph(
        tr("Argonaut is a file manager in the spirit of GNOME Files: simple to use, quick with big folders, "
           "and careful with your files. It shares bookmarks, recent files, thumbnails and the Trash with "
           "your desktop."),
        page));

    // Four cards: what it's good at.
    auto *grid = new QGridLayout;
    grid->setSpacing(8);
    const QList<std::pair<QString, QString>> cards = {
        {tr("Fast"), tr("A folder of 20,000 files opens in about 0.15 seconds.")},
        {tr("Careful"), tr("Never overwrites, and undoes almost everything.")},
        {tr("Honest about types"), tr("Spots files whose name lies about what they are.")},
        {tr("Batteries included"), tr("Split view, Quick Look, disk usage, archives and a terminal.")},
    };
    for (int i = 0; i < cards.size(); ++i) {
        auto *card = new QWidget(page);
        card->setObjectName("aboutCard");
        auto *cardLayout = new QVBoxLayout(card);
        cardLayout->setContentsMargins(12, 10, 12, 10);
        cardLayout->setSpacing(2);
        auto *heading = new QLabel(cards[i].first, card);
        heading->setObjectName("aboutCardHeading");
        cardLayout->addWidget(heading);
        cardLayout->addWidget(paragraph(cards[i].second, card, "aboutCardText"));
        grid->addWidget(card, i / 2, i % 2);
    }
    layout->addLayout(grid);
    layout->addWidget(paragraph(tr("Named after the Argonauts, who sailed on the <i>Argo</i> into the unknown, "
                                   "and the <i>argonauta</i>, whose shell is called the paper nautilus."),
                                page, "aboutDim"));
    layout->addWidget(paragraph(tr("<a href=\"https://github.com/jgiannop/argonaut\">Website</a> · "
                                   "<a href=\"https://github.com/jgiannop/argonaut/issues\">Report a problem</a>"),
                                page));
    layout->addStretch();
    return page;
}

QWidget *AboutDialog::buildSystemPage()
{
    auto *page = new QWidget(m_pages);
    page->setObjectName("aboutCard");
    auto *grid = new QGridLayout(page);
    grid->setContentsMargins(14, 12, 14, 12);
    grid->setHorizontalSpacing(16);
    grid->setVerticalSpacing(5);
    int row = 0;
    for (const Detail &d : details()) {
        auto *label = new QLabel(d.label, page);
        label->setObjectName("aboutLabel");
        label->setAlignment(Qt::AlignRight | Qt::AlignTop);
        // Long paths are shortened in the middle; the tooltip and Copy Details have them whole.
        auto *value = new QLabel(page);
        const QString shown = value->fontMetrics().elidedText(d.value, Qt::ElideMiddle, 270);
        value->setText(shown);
        if (shown != d.value)
            value->setToolTip(d.value);
        value->setObjectName("aboutValue");
        value->setTextInteractionFlags(Qt::TextSelectableByMouse);
        grid->addWidget(label, row, 0);
        grid->addWidget(value, row, 1);
        ++row;
    }
    grid->setColumnStretch(1, 1);
    grid->setRowStretch(row, 1);
    return page;
}

QWidget *AboutDialog::buildCreditsPage()
{
    auto *page = new QWidget(m_pages);
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(10);
    QString built = tr("<a href=\"https://www.qt.io\">Qt</a> %1 for the interface, the same on every system");
#if defined(ARGONAUT_LIBARCHIVE_VERSION)
    built += tr("<br><a href=\"https://libarchive.org\">libarchive</a> for compressing and extracting");
#endif
#if defined(ARGONAUT_TERMINAL)
    built += tr("<br><a href=\"https://www.leonerd.org.uk/code/libvterm/\">libvterm</a> for the terminal");
#endif
#if defined(Q_OS_LINUX)
    built += tr("<br><a href=\"https://github.com/containers/bubblewrap\">bubblewrap</a> to keep thumbnailers sandboxed");
#endif
    layout->addWidget(card(tr("Built with"), built.arg(QString::fromLatin1(qVersion())), page));
    layout->addWidget(card(tr("Inspired by"),
                           tr("<a href=\"https://apps.gnome.org/Nautilus/\">GNOME Files</a> and the "
                              "<a href=\"https://gnome.pages.gitlab.gnome.org/libadwaita/\">libadwaita</a> look, "
                              "redrawn with Qt alone. Terminal colours from GNOME Console."),
                           page));
    layout->addWidget(card(tr("License"),
                           tr("Copyright © 2026 johngiannop. Argonaut is free software: you can share and change it "
                              "under the <a href=\"https://www.gnu.org/licenses/gpl-3.0.html\">GNU General Public "
                              "License, version 3 or later</a>. It comes with no warranty."),
                           page));
    layout->addWidget(card(tr("Works with your desktop"),
                           tr("Follows the freedesktop.org standards for the Trash, thumbnails, recent files "
                              "and default apps, so it shares them with other apps."),
                           page));
    layout->addStretch();
    return page;
}
