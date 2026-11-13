#include "PathBar.h"

#include "DirectoryModel.h"
#include "Trash.h"

#include <QDir>
#include <QEvent>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QStackedWidget>
#include <QToolButton>

PathBar::PathBar(QWidget *parent)
    : QFrame(parent)
{
    setObjectName("pathBar");
    setFixedHeight(32);
    setCursor(Qt::IBeamCursor);

    auto *row = new QHBoxLayout(this);
    row->setContentsMargins(2, 2, 2, 2);
    row->setSpacing(0);

    m_stack = new QStackedWidget(this);
    m_stack->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);

    m_crumbs = new QWidget(m_stack);
    m_crumbs->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_crumbs->installEventFilter(this);
    m_crumbLayout = new QHBoxLayout(m_crumbs);
    m_crumbLayout->setContentsMargins(2, 0, 0, 0);
    m_crumbLayout->setSpacing(0);

    m_edit = new QLineEdit(m_stack);
    m_edit->setObjectName("pathEdit");
    m_edit->installEventFilter(this);
    connect(m_edit, &QLineEdit::returnPressed, this, [this] {
        QString text = m_edit->text().trimmed();
        if (text == "~" || text.startsWith("~/"))
            text.replace(0, 1, QDir::homePath());
        stopEditing();
        if (!text.isEmpty())
            emit pathActivated(text);
    });

    m_stack->addWidget(m_crumbs);
    m_stack->addWidget(m_edit);
    row->addWidget(m_stack, 1);

    m_menuButton = new QToolButton(this);
    m_menuButton->setObjectName("flat");
    m_menuButton->setToolTip(tr("Folder menu"));
    m_menuButton->setPopupMode(QToolButton::InstantPopup);
    m_menuButton->setCursor(Qt::ArrowCursor);
    m_menuButton->setIconSize(QSize(16, 16));
    row->addWidget(m_menuButton);
}

void PathBar::setPath(const QString &path)
{
    if (path == m_path)
        return;
    m_path = path;
    stopEditing();
    rebuild();
}

void PathBar::setTheme(const Theme &theme)
{
    m_theme = theme;
    m_menuButton->setIcon(glyphIcon(Glyph::Kebab, theme.fg));
    rebuild();
}

void PathBar::startEditing()
{
    m_edit->setText(QDir::toNativeSeparators(m_path));
    m_stack->setCurrentWidget(m_edit);
    m_edit->setFocus(Qt::ShortcutFocusReason);
    m_edit->selectAll();
}

void PathBar::stopEditing()
{
    if (m_stack->currentWidget() != m_crumbs)
        m_stack->setCurrentWidget(m_crumbs);
}

void PathBar::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        startEditing();
        event->accept();
        return;
    }
    QFrame::mousePressEvent(event);
}

bool PathBar::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_crumbs && event->type() == QEvent::Resize) {
        fitCrumbs();
    } else if (watched == m_edit) {
        if (event->type() == QEvent::FocusOut) {
            stopEditing();
        } else if (event->type() == QEvent::KeyPress
                   && static_cast<QKeyEvent *>(event)->key() == Qt::Key_Escape) {
            stopEditing();
            return true;
        }
    }
    return QFrame::eventFilter(watched, event);
}

void PathBar::rebuild()
{
    for (const auto &group : std::as_const(m_groups))
        qDeleteAll(group);
    m_groups.clear();
    while (QLayoutItem *item = m_crumbLayout->takeAt(0))
        delete item;

    if (m_path.isEmpty())
        return;

    struct Segment {
        QString label;
        QString path;
        Glyph glyph;
        bool hasIcon;
    };
    QList<Segment> segments;

    const QString home = QDir::cleanPath(QDir::homePath());
    const QString trash = Trash::filesDirOf(m_path);
    QString base;
    QString rest;
    if (DirectoryModel::isVirtualPath(m_path)) {
        // Recent / Starred: one place, not a folder.
        const bool recent = DirectoryModel::placeOf(m_path) == DirectoryModel::Place::Recent;
        segments.append({recent ? tr("Recent") : tr("Starred"), m_path, recent ? Glyph::Recent : Glyph::Star, true});
    } else if (!trash.isEmpty()) {
        // The Trash is one place, wherever its folders really are.
        segments.append({tr("Trash"), Trash::homeFilesDir(), Glyph::Trash, true});
        base = trash;
        rest = m_path.mid(trash.size());
    } else if (m_path == home || m_path.startsWith(home + '/')) {
        segments.append({tr("Home"), home, Glyph::Home, true});
        base = home;
        rest = m_path.mid(home.size());
    } else {
        // "/" on Unix, "C:/" on Windows.
        const int slash = m_path.indexOf('/');
        base = m_path.left(slash + 1);
        const QString label = base == "/" ? tr("Computer") : base.left(base.size() - 1);
        segments.append({label, base, Glyph::Drive, true});
        rest = m_path.mid(base.size());
    }
    for (const QString &part : rest.split('/', Qt::SkipEmptyParts)) {
        base = base.endsWith('/') ? base + part : base + '/' + part;
        segments.append({part, base, Glyph::Home, false});
    }

    for (int i = 0; i < segments.size(); ++i) {
        const Segment &seg = segments.at(i);
        const bool last = i == segments.size() - 1;

        auto *button = new QToolButton(m_crumbs);
        button->setObjectName(last ? "crumbCurrent" : "crumb");
        button->setText(seg.label);
        button->setCursor(Qt::ArrowCursor);
        button->setAutoRaise(true);
        if (seg.hasIcon) {
            button->setIcon(glyphIcon(seg.glyph, last ? m_theme.fg : m_theme.dimFg));
            button->setIconSize(QSize(16, 16));
            button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        }
        const QString target = seg.path;
        connect(button, &QToolButton::clicked, this, [this, target] { emit pathActivated(target); });
        m_crumbLayout->addWidget(button);

        QList<QWidget *> group{button};
        if (!last) {
            auto *sep = new QLabel("/", m_crumbs);
            sep->setObjectName("crumbSep");
            sep->setContentsMargins(2, 0, 2, 0);
            m_crumbLayout->addWidget(sep);
            group.append(sep);
        }
        m_groups.append(group);
    }
    m_crumbLayout->addStretch(1);
    fitCrumbs();
}

// Hide leading crumbs until the rest fits, always keeping the current one.
void PathBar::fitCrumbs()
{
    if (m_groups.isEmpty())
        return;
    const QMargins m = m_crumbLayout->contentsMargins();
    const int available = m_crumbs->width() - m.left() - m.right();

    int total = 0;
    QList<int> widths;
    for (const auto &group : std::as_const(m_groups)) {
        int w = 0;
        for (QWidget *widget : group)
            w += widget->sizeHint().width();
        widths.append(w);
        total += w;
    }
    for (int i = 0; i < m_groups.size(); ++i) {
        const bool hide = total > available && i < m_groups.size() - 1;
        for (QWidget *widget : m_groups.at(i))
            widget->setVisible(!hide);
        if (hide)
            total -= widths.at(i);
    }
}
