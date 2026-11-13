#include "MainWindow.h"

#include "FileItemDelegate.h"
#include "FileOperations.h"
#include "HeaderBar.h"
#include "OperationsPanel.h"
#include "PathBar.h"
#include "PreferencesDialog.h"
#include "Thumbnailer.h"
#include "Toast.h"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QDesktopServices>
#include <QDir>
#include <QEvent>
#include <QFileInfo>
#include <QFileSystemModel>
#include <QHBoxLayout>
#include <QItemSelectionModel>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QListWidget>
#include <QLocale>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPushButton>
#include <QPainter>
#include <QPointer>
#include <QSplitter>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <QWindow>

namespace {

constexpr int PathRole = Qt::UserRole;
constexpr int SeparatorRole = Qt::UserRole + 1;
constexpr int ResizeMargin = 5;

// Separator rows are drawn as a thin line; other rows are drawn normally so
// the style sheet's rounded hover/selection applies.
class SidebarDelegate : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    QColor lineColor;

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        if (!index.data(SeparatorRole).toBool()) {
            QStyledItemDelegate::paint(painter, option, index);
            return;
        }
        const int y = option.rect.center().y();
        painter->save();
        painter->setPen(QPen(lineColor, 1));
        painter->drawLine(option.rect.left() + 4, y, option.rect.right() - 4, y);
        painter->restore();
    }

    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        if (index.data(SeparatorRole).toBool())
            return QSize(0, 13);
        return QStyledItemDelegate::sizeHint(option, index);
    }
};

QString styleSheetFor(const Theme &t)
{
    QString css = QStringLiteral(R"(
QWidget#root { background: @outline; }
QWidget#sidebarPane { background: @sidebar; }
QWidget#contentPane { background: @window; }
QLabel#title { color: @fg; font-weight: bold; }

QToolButton#flat { background: transparent; border: none; border-radius: 6px;
    padding: 5px; color: @fg; }
QToolButton#flat:hover { background: @hover; }
QToolButton#flat:pressed, QToolButton#flat:checked { background: @pressed; }
QToolButton#flat::menu-indicator { image: none; width: 0px; }

QToolButton#win { background: @button; border: none; border-radius: 12px; }
QToolButton#win:hover { background: @buttonHover; }
QToolButton#win:pressed { background: @pressed; }

QFrame#pathBar { background: @pathBg; border-radius: 8px; }
QToolButton#crumb, QToolButton#crumbCurrent { background: transparent; border: none;
    border-radius: 6px; padding: 3px 7px; color: @dim; }
QToolButton#crumbCurrent { color: @fg; font-weight: bold; }
QToolButton#crumb:hover, QToolButton#crumbCurrent:hover { background: @hover; color: @fg; }
QLabel#crumbSep { color: @dim; }
QLineEdit#pathEdit { background: transparent; border: none; color: @fg; padding: 0px 8px;
    selection-background-color: @accent; }
QLineEdit#searchEdit { background: @pathBg; border: none; border-radius: 8px; color: @fg;
    padding: 0px 10px; selection-background-color: @accent; }

QListWidget#sidebar { background: @sidebar; color: @fg; border: none; padding: 0px 6px 6px 6px;
    outline: 0; }
QListWidget#sidebar::item { height: 36px; padding-left: 8px; border: none; border-radius: 6px;
    margin: 1px 0px; color: @fg; }
QListWidget#sidebar::item:hover { background: @hover; }
QListWidget#sidebar::item:selected { background: @selected; color: @fg; }

QListView#files { background: @window; border: none; outline: 0; padding: 6px 6px; }
QLabel#floatingBar { background: @popup; color: @fg; border: 1px solid @border;
    border-radius: 8px; padding: 5px 12px; }

QSplitter#mainSplitter::handle { background: @splitter; }

QScrollBar:vertical { background: transparent; width: 10px; margin: 2px 0px; }
QScrollBar::handle:vertical { background: @scroll; border-radius: 3px; min-height: 32px;
    margin: 0px 2px; }
QScrollBar::handle:vertical:hover { background: @scrollHover; }
QScrollBar:horizontal { background: transparent; height: 10px; margin: 0px 2px; }
QScrollBar::handle:horizontal { background: @scroll; border-radius: 3px; min-width: 32px;
    margin: 2px 0px; }
QScrollBar::handle:horizontal:hover { background: @scrollHover; }
QScrollBar::add-line, QScrollBar::sub-line { width: 0px; height: 0px; }
QScrollBar::add-page, QScrollBar::sub-page { background: none; }

QMenu { background: @popup; color: @fg; border: 1px solid @border; border-radius: 10px;
    padding: 6px; }
QMenu::item { padding: 7px 24px 7px 28px; border-radius: 6px; }
QMenu::item:selected { background: @hover; }
QMenu::item:disabled { color: @dim; }
QMenu::separator { height: 1px; background: @border; margin: 5px 6px; }

QFrame#opsPanel { background: @popup; border: 1px solid @border; border-radius: 12px; }
QLabel#opsTitle { color: @fg; }
QLabel#opsDetail { color: @dim; }
QProgressBar#opsProgress { background: @pathBg; border: none; border-radius: 3px; }
QProgressBar#opsProgress::chunk { background: @accent; border-radius: 3px; }
QFrame#toast { background: @popup; border: 1px solid @border; border-radius: 20px; }
QLabel#toastText { color: @fg; }
QToolButton#toastAction { background: @button; color: @fg; border: none; border-radius: 14px;
    padding: 5px 14px; font-weight: bold; }
QToolButton#toastAction:hover { background: @buttonHover; }

QDialog#preferences { background: @window; }
QLabel#prefHeading { color: @fg; font-weight: bold; font-size: 15px; }
QLabel#prefHint { color: @dim; }
QToolButton#themeCard { background: transparent; color: @fg; border: 2px solid transparent;
    border-radius: 12px; padding: 8px 8px 6px 8px; }
QToolButton#themeCard:hover { background: @hover; }
QToolButton#themeCard:checked { border-color: @accent; }
)");
    const QList<std::pair<const char *, QColor>> tokens = {
        {"@outline", t.outline},   {"@sidebar", t.sidebar},       {"@window", t.window},
        {"@fg", t.fg},             {"@dim", t.dimFg},             {"@hover", t.hover},
        {"@pressed", t.pressed},   {"@selected", t.selected},     {"@buttonHover", t.buttonHover},
        {"@button", t.button},     {"@pathBg", t.pathBg},         {"@accent", t.accent},
        {"@popup", t.popup},       {"@border", t.border},         {"@scrollHover", t.scrollHover},
        {"@scroll", t.scroll},     {"@splitter", t.dark ? t.sidebar : t.border},
    };
    // Longer tokens first where one is a prefix of another (@button/@buttonHover…).
    for (const auto &[token, color] : tokens)
        css.replace(QLatin1String(token), cssColor(color));
    return css;
}

} // namespace

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    m_csd = qEnvironmentVariableIsEmpty("KAFTILOUS_NATIVE_DECORATIONS");
#if defined(Q_OS_MACOS)
    m_csd = false; // keep the native traffic-light title bar on macOS
#endif
    if (m_csd)
        setWindowFlag(Qt::FramelessWindowHint);

    resize(1150, 720);
    setWindowTitle("Kaftilous");

    m_ops = new FileOperations(this);
    connect(m_ops, &FileOperations::jobFinished, this, &MainWindow::onJobFinished);
    connect(m_ops, &FileOperations::undoChanged, this, &MainWindow::updateEditActions);
    connect(QApplication::clipboard(), &QClipboard::dataChanged, this,
            &MainWindow::updateEditActions);

    setupActions();
    setupUi();
    applyTheme(true);
    updateWindowState();

    if (m_csd) {
        winId(); // create the native window so we can watch its mouse events
        if (QWindow *handle = windowHandle())
            handle->installEventFilter(this);
    }
}

// ---------------------------------------------------------------- setup ---

void MainWindow::setupActions()
{
    m_backAct = new QAction(tr("Back"), this);
    m_backAct->setShortcuts({QKeySequence::Back, QKeySequence(Qt::ALT | Qt::Key_Left)});
    connect(m_backAct, &QAction::triggered, this, &MainWindow::goBack);

    m_forwardAct = new QAction(tr("Forward"), this);
    m_forwardAct->setShortcuts({QKeySequence::Forward, QKeySequence(Qt::ALT | Qt::Key_Right)});
    connect(m_forwardAct, &QAction::triggered, this, &MainWindow::goForward);

    m_upAct = new QAction(tr("Up"), this);
    m_upAct->setShortcut(QKeySequence(Qt::ALT | Qt::Key_Up));
    connect(m_upAct, &QAction::triggered, this, &MainWindow::goUp);

    auto *homeAct = new QAction(tr("Home"), this);
    homeAct->setShortcut(QKeySequence(Qt::ALT | Qt::Key_Home));
    connect(homeAct, &QAction::triggered, this, [this] { navigateTo(QDir::homePath()); });

    auto *locationAct = new QAction(tr("Enter Location"), this);
    locationAct->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_L));
    connect(locationAct, &QAction::triggered, this, [this] {
        setSearchActive(false);
        m_pathBar->startEditing();
    });

    m_hiddenAct = new QAction(tr("Show Hidden Files"), this);
    m_hiddenAct->setCheckable(true);
    m_hiddenAct->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_H));
    connect(m_hiddenAct, &QAction::toggled, this, &MainWindow::setShowHidden);

    m_searchAct = new QAction(tr("Search"), this);
    m_searchAct->setCheckable(true);
    m_searchAct->setShortcut(QKeySequence::Find);
    connect(m_searchAct, &QAction::toggled, this, &MainWindow::setSearchActive);

    auto *gridAct = new QAction(tr("Grid View"), this);
    gridAct->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_1));
    connect(gridAct, &QAction::triggered, this, [this] { setGridMode(true); });
    auto *listAct = new QAction(tr("List View"), this);
    listAct->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_2));
    connect(listAct, &QAction::triggered, this, [this] { setGridMode(false); });

    m_copyLocationAct = new QAction(tr("Copy Location"), this);
    connect(m_copyLocationAct, &QAction::triggered, this, [this] {
        QApplication::clipboard()->setText(QDir::toNativeSeparators(m_current));
        showMessage(tr("Location copied"));
    });

    m_prefsAct = new QAction(tr("Preferences"), this);
    m_prefsAct->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Comma));
    connect(m_prefsAct, &QAction::triggered, this, [this] {
        static QPointer<PreferencesDialog> dialog;
        if (!dialog) {
            dialog = new PreferencesDialog(this);
            dialog->setAttribute(Qt::WA_DeleteOnClose);
        }
        dialog->show();
        dialog->raise();
        dialog->activateWindow();
    });

    m_openAct = new QAction(tr("Open"), this);
    connect(m_openAct, &QAction::triggered, this, &MainWindow::openSelection);

    m_cutAct = new QAction(tr("Cut"), this);
    m_cutAct->setShortcut(QKeySequence::Cut);
    connect(m_cutAct, &QAction::triggered, this, [this] { copySelection(true); });

    m_copyAct = new QAction(tr("Copy"), this);
    m_copyAct->setShortcut(QKeySequence::Copy);
    connect(m_copyAct, &QAction::triggered, this, [this] { copySelection(false); });

    m_pasteAct = new QAction(tr("Paste"), this);
    m_pasteAct->setShortcut(QKeySequence::Paste);
    connect(m_pasteAct, &QAction::triggered, this, &MainWindow::paste);

    m_trashAct = new QAction(tr("Move to Trash"), this);
    m_trashAct->setShortcut(QKeySequence::Delete);
    connect(m_trashAct, &QAction::triggered, this, &MainWindow::trashSelection);

    m_undoAct = new QAction(tr("Undo"), this);
    m_undoAct->setShortcut(QKeySequence::Undo);
    connect(m_undoAct, &QAction::triggered, m_ops, &FileOperations::undo);

    m_aboutAct = new QAction(tr("About Kaftilous"), this);
    connect(m_aboutAct, &QAction::triggered, this, [this] {
        QMessageBox::about(this, tr("About Kaftilous"),
                           tr("<b>Kaftilous</b> %1<br>A fast, cross-platform file manager.")
                               .arg(QApplication::applicationVersion()));
    });

    addActions({m_backAct, m_forwardAct, m_upAct, homeAct, locationAct, m_hiddenAct, m_searchAct,
                gridAct, listAct, m_prefsAct, m_cutAct, m_copyAct, m_pasteAct, m_trashAct,
                m_undoAct});
}

QToolButton *MainWindow::makeButton(const QString &toolTip, const char *objectName)
{
    auto *button = new QToolButton(this);
    button->setObjectName(objectName);
    button->setToolTip(toolTip);
    button->setIconSize(QSize(16, 16));
    button->setFocusPolicy(Qt::NoFocus);
    if (qstrcmp(objectName, "win") == 0)
        button->setFixedSize(24, 24);
    else
        button->setFixedSize(32, 32);
    return button;
}

// Rounded popup menu (needs a translucent, frameless popup window).
QMenu *MainWindow::makeMenu()
{
    auto *menu = new QMenu(this);
    menu->setWindowFlags(menu->windowFlags() | Qt::FramelessWindowHint
                         | Qt::NoDropShadowWindowHint);
    menu->setAttribute(Qt::WA_TranslucentBackground);
    return menu;
}

void MainWindow::setupUi()
{
    m_model = new QFileSystemModel(this);
    m_model->setRootPath(QString()); // watch the whole filesystem lazily
    m_model->setNameFilterDisables(false);
    updateFilter();
    connect(m_model, &QFileSystemModel::directoryLoaded, this, [this](const QString &path) {
        if (QDir::cleanPath(path) == m_current)
            updateFloatingBar();
    });

    m_thumbnailer = new Thumbnailer(this);

    m_root = new QWidget(this);
    m_root->setObjectName("root");
    m_root->setAttribute(Qt::WA_StyledBackground);
    auto *rootLayout = new QVBoxLayout(m_root);
    rootLayout->setSpacing(0);

    m_splitter = new QSplitter(m_root);
    m_splitter->setObjectName("mainSplitter");
    m_splitter->addWidget(buildSidebarPane());
    m_splitter->addWidget(buildContentPane());
    m_splitter->setChildrenCollapsible(false);
    m_splitter->setHandleWidth(1);
    m_splitter->setStretchFactor(1, 1);
    m_splitter->setSizes({230, 920});
    rootLayout->addWidget(m_splitter);

    setCentralWidget(m_root);
}

QWidget *MainWindow::buildSidebarPane()
{
    auto *pane = new QWidget(this);
    pane->setObjectName("sidebarPane");
    pane->setAttribute(Qt::WA_StyledBackground);
    pane->setMinimumWidth(170);
    auto *layout = new QVBoxLayout(pane);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto *header = new HeaderBar(pane);
    m_searchButton = makeButton(tr("Search (Ctrl+F)"));
    m_searchButton->setCheckable(true);
    connect(m_searchButton, &QToolButton::toggled, m_searchAct, &QAction::setChecked);
    connect(m_searchAct, &QAction::toggled, m_searchButton, &QToolButton::setChecked);

    m_titleLabel = new QLabel(tr("Kaftilous"), header);
    m_titleLabel->setObjectName("title");
    m_titleLabel->setAlignment(Qt::AlignCenter);

    m_menuButton = makeButton(tr("Main menu"));
    m_menuButton->setPopupMode(QToolButton::InstantPopup);
    QMenu *mainMenu = makeMenu();
    mainMenu->addAction(m_undoAct);
    mainMenu->addSeparator();
    mainMenu->addAction(m_hiddenAct);
    mainMenu->addSeparator();
    mainMenu->addAction(m_prefsAct);
    mainMenu->addAction(m_aboutAct);
    m_menuButton->setMenu(mainMenu);

    header->row()->addWidget(m_searchButton);
    header->row()->addWidget(m_titleLabel, 1);
    header->row()->addWidget(m_menuButton);
    layout->addWidget(header);

    m_sidebar = new QListWidget(pane);
    m_sidebar->setObjectName("sidebar");
    m_sidebar->setItemDelegate(new SidebarDelegate(m_sidebar));
    m_sidebar->setIconSize(QSize(16, 16));
    m_sidebar->setFrameShape(QFrame::NoFrame);
    m_sidebar->setFocusPolicy(Qt::NoFocus);
    m_sidebar->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_sidebar->setTextElideMode(Qt::ElideRight);
    connect(m_sidebar, &QListWidget::itemClicked, this, [this](QListWidgetItem *item) {
        const QString path = item->data(PathRole).toString();
        if (path.isEmpty() || !navigateTo(path))
            syncSidebarSelection();
    });
    layout->addWidget(m_sidebar, 1);
    return pane;
}

QWidget *MainWindow::buildContentPane()
{
    auto *pane = new QWidget(this);
    pane->setObjectName("contentPane");
    pane->setAttribute(Qt::WA_StyledBackground);
    auto *layout = new QVBoxLayout(pane);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto *header = new HeaderBar(pane);
    QHBoxLayout *row = header->row();

    m_backButton = makeButton(tr("Back (Alt+Left)"));
    connect(m_backButton, &QToolButton::clicked, m_backAct, &QAction::trigger);
    m_forwardButton = makeButton(tr("Forward (Alt+Right)"));
    connect(m_forwardButton, &QToolButton::clicked, m_forwardAct, &QAction::trigger);
    row->addWidget(m_backButton);
    row->addWidget(m_forwardButton);
    row->addSpacing(4);

    m_pathBar = new PathBar(header);
    connect(m_pathBar, &PathBar::pathActivated, this, [this](const QString &p) { navigateTo(p); });
    QMenu *folderMenu = makeMenu();
    folderMenu->addAction(m_pasteAct);
    folderMenu->addSeparator();
    folderMenu->addAction(m_copyLocationAct);
    folderMenu->addSeparator();
    folderMenu->addAction(m_hiddenAct);
    m_pathBar->menuButton()->setMenu(folderMenu);

    m_searchEdit = new QLineEdit(header);
    m_searchEdit->setObjectName("searchEdit");
    m_searchEdit->setFixedHeight(32);
    m_searchEdit->setPlaceholderText(tr("Search current folder"));
    m_searchEdit->installEventFilter(this);
    connect(m_searchEdit, &QLineEdit::textChanged, this, &MainWindow::updateFilter);

    m_locationStack = new QStackedWidget(header);
    m_locationStack->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    m_locationStack->setFixedHeight(32);
    m_locationStack->addWidget(m_pathBar);
    m_locationStack->addWidget(m_searchEdit);
    row->addWidget(m_locationStack, 1);
    row->addSpacing(4);

    m_viewButton = makeButton(QString());
    connect(m_viewButton, &QToolButton::clicked, this, [this] { setGridMode(!m_grid); });
    row->addWidget(m_viewButton);

    if (m_csd) {
        row->addSpacing(10);
        m_minButton = makeButton(tr("Minimize"), "win");
        connect(m_minButton, &QToolButton::clicked, this, &QWidget::showMinimized);
        m_maxButton = makeButton(tr("Maximize"), "win");
        connect(m_maxButton, &QToolButton::clicked, this,
                [this] { isMaximized() ? showNormal() : showMaximized(); });
        m_closeButton = makeButton(tr("Close"), "win");
        connect(m_closeButton, &QToolButton::clicked, this, &QWidget::close);
        for (QToolButton *b : {m_minButton, m_maxButton, m_closeButton}) {
            row->addWidget(b);
            row->addSpacing(6);
        }
        row->addSpacing(-6);
    }
    layout->addWidget(header);

    m_delegate = new FileItemDelegate(m_thumbnailer, this);
    m_view = new QListView(pane);
    m_view->setObjectName("files");
    m_view->setModel(m_model);
    m_view->setItemDelegate(m_delegate);
    m_view->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_view->setUniformItemSizes(true);
    m_view->setFrameShape(QFrame::NoFrame);
    m_view->setMouseTracking(true);
    m_view->viewport()->setAttribute(Qt::WA_Hover);
    m_view->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_view->installEventFilter(this);
    connect(m_view, &QListView::activated, this, &MainWindow::openIndex);
    connect(m_view->selectionModel(), &QItemSelectionModel::selectionChanged, this,
            &MainWindow::updateFloatingBar);
    connect(m_view->selectionModel(), &QItemSelectionModel::selectionChanged, this,
            &MainWindow::updateEditActions);
    m_view->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_view, &QWidget::customContextMenuRequested, this, &MainWindow::showContextMenu);
    connect(m_thumbnailer, &Thumbnailer::thumbnailReady, m_view->viewport(),
            qOverload<>(&QWidget::update));
    layout->addWidget(m_view, 1);

    m_floatingBar = new QLabel(m_view);
    m_floatingBar->setObjectName("floatingBar");
    m_floatingBar->hide();
    m_messageTimer = new QTimer(this);
    m_messageTimer->setSingleShot(true);
    m_messageTimer->setInterval(3000);
    connect(m_messageTimer, &QTimer::timeout, this, [this] {
        m_message.clear();
        updateFloatingBar();
    });

    m_opsPanel = new OperationsPanel(m_ops, m_view);
    m_toast = new Toast(m_view);
    connect(m_opsPanel, &OperationsPanel::layoutChanged, this, [this] {
        m_toast->setAvoidRect(m_opsPanel->isVisible() ? m_opsPanel->geometry() : QRect());
    });
    updateEditActions();

    setGridMode(true);
    return pane;
}

// ---------------------------------------------------------------- theme ---

void MainWindow::applyTheme(bool force)
{
    // Use the application palette: our own style sheet changes widget palettes.
    const Theme theme = Theme::fromPalette(QApplication::palette());
    if (!force && m_themeDark == int(theme.dark))
        return;
    m_theme = theme;
    m_themeDark = int(theme.dark);

    setStyleSheet(styleSheetFor(m_theme));

    m_backButton->setIcon(glyphIcon(Glyph::ChevronLeft, m_theme.fg));
    m_forwardButton->setIcon(glyphIcon(Glyph::ChevronRight, m_theme.fg));
    m_searchButton->setIcon(glyphIcon(Glyph::Search, m_theme.fg));
    m_menuButton->setIcon(glyphIcon(Glyph::Menu, m_theme.fg));
    m_viewButton->setIcon(glyphIcon(m_grid ? Glyph::List : Glyph::Grid, m_theme.fg));
    if (m_csd) {
        m_minButton->setIcon(glyphIcon(Glyph::Minimize, m_theme.fg));
        m_closeButton->setIcon(glyphIcon(Glyph::Close, m_theme.fg));
        updateWindowState();
    }

    static_cast<SidebarDelegate *>(m_sidebar->itemDelegate())->lineColor = m_theme.border;
    m_pathBar->setTheme(m_theme);
    m_delegate->setTheme(m_theme);
    m_opsPanel->setTheme(m_theme);
    m_toast->setTheme(m_theme);
    m_view->viewport()->update();

    populateSidebar();
    syncSidebarSelection();
}

void MainWindow::changeEvent(QEvent *event)
{
    QMainWindow::changeEvent(event);
    // Widgets with a style sheet get PaletteChange rather than
    // ApplicationPaletteChange. applyTheme() reads the application palette
    // and returns early if light/dark didn't change, so this can't loop.
    if (event->type() == QEvent::ApplicationPaletteChange
        || event->type() == QEvent::PaletteChange)
        applyTheme();
    else if (event->type() == QEvent::WindowStateChange)
        updateWindowState();
}

// -------------------------------------------------------------- sidebar ---

void MainWindow::populateSidebar()
{
    m_sidebar->clear();
    const QColor iconColor = m_theme.fg;

    auto addPlace = [&](const QString &name, const QString &path, Glyph glyph) {
        auto *item = new QListWidgetItem(glyphIcon(glyph, iconColor), name, m_sidebar);
        item->setData(PathRole, path);
        item->setToolTip(QDir::toNativeSeparators(path));
    };
    auto addSeparator = [&] {
        auto *sep = new QListWidgetItem(m_sidebar);
        sep->setFlags(Qt::NoItemFlags);
        sep->setData(SeparatorRole, true);
    };

    const struct {
        QStandardPaths::StandardLocation loc;
        const char *name;
        Glyph glyph;
    } places[] = {
        {QStandardPaths::HomeLocation, QT_TR_NOOP("Home"), Glyph::Home},
        {QStandardPaths::DesktopLocation, QT_TR_NOOP("Desktop"), Glyph::Desktop},
        {QStandardPaths::DocumentsLocation, QT_TR_NOOP("Documents"), Glyph::Documents},
        {QStandardPaths::DownloadLocation, QT_TR_NOOP("Downloads"), Glyph::Downloads},
        {QStandardPaths::MusicLocation, QT_TR_NOOP("Music"), Glyph::Music},
        {QStandardPaths::PicturesLocation, QT_TR_NOOP("Pictures"), Glyph::Pictures},
        {QStandardPaths::MoviesLocation, QT_TR_NOOP("Videos"), Glyph::Videos},
    };
    QStringList added;
    for (const auto &p : places) {
        const QString path = QStandardPaths::writableLocation(p.loc);
        if (path.isEmpty() || added.contains(path) || !QFileInfo(path).isDir())
            continue;
        added << path;
        addPlace(tr(p.name), path, p.glyph);
    }

#if defined(Q_OS_LINUX) || defined(Q_OS_FREEBSD)
    // Freedesktop trash folder (browse only for now).
    const QString trash = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)
                          + "/Trash/files";
    if (QFileInfo(trash).isDir())
        addPlace(tr("Trash"), trash, Glyph::Trash);
#endif

    addSeparator();

    for (const QStorageInfo &vol : QStorageInfo::mountedVolumes()) {
        if (!vol.isValid() || !vol.isReady())
            continue;
        const QString root = vol.rootPath();
#if defined(Q_OS_LINUX)
        // Skip system/snap mounts; keep "/" and user-visible drives.
        if (root != "/" && !root.startsWith("/media/") && !root.startsWith("/mnt/")
            && !root.startsWith("/run/media/"))
            continue;
#elif defined(Q_OS_MACOS)
        if (root.startsWith("/System/Volumes"))
            continue;
#endif
        const bool system = root == QDir::rootPath();
        addPlace(system ? tr("Computer") : vol.displayName(), root,
                 system ? Glyph::Drive : Glyph::Removable);
    }
}

// Highlight the sidebar entry matching the current folder, like Nautilus.
void MainWindow::syncSidebarSelection()
{
    for (int i = 0; i < m_sidebar->count(); ++i) {
        QListWidgetItem *item = m_sidebar->item(i);
        const QString path = item->data(PathRole).toString();
        if (!path.isEmpty() && QDir::cleanPath(path) == m_current) {
            m_sidebar->setCurrentItem(item);
            return;
        }
    }
    m_sidebar->clearSelection();
    m_sidebar->setCurrentRow(-1);
}

// ----------------------------------------------------------- navigation ---

bool MainWindow::navigateTo(const QString &path, bool addToHistory)
{
    const QFileInfo info(path);
    if (!info.exists() || !info.isDir()) {
        showMessage(tr("Folder not found: %1").arg(path));
        return false;
    }

    const QString clean = QDir::cleanPath(info.absoluteFilePath());
    if (clean == m_current)
        return true;

    setSearchActive(false);
    if (addToHistory && !m_current.isEmpty()) {
        m_backHistory.push(m_current);
        m_forwardHistory.clear();
    }
    m_current = clean;

    m_view->setRootIndex(m_model->index(clean));
    m_view->scrollToTop();
    m_pathBar->setPath(clean);

    const QString title = info.fileName().isEmpty() ? QDir::toNativeSeparators(clean)
                                                    : info.fileName();
    setWindowTitle(tr("%1 — Kaftilous").arg(title));

    syncSidebarSelection();
    updateActions();
    updateFloatingBar();
    return true;
}

void MainWindow::goBack()
{
    if (m_backHistory.isEmpty())
        return;
    const QString previous = m_current;
    if (navigateTo(m_backHistory.pop(), false))
        m_forwardHistory.push(previous);
    updateActions();
}

void MainWindow::goForward()
{
    if (m_forwardHistory.isEmpty())
        return;
    const QString previous = m_current;
    if (navigateTo(m_forwardHistory.pop(), false))
        m_backHistory.push(previous);
    updateActions();
}

void MainWindow::goUp()
{
    QDir dir(m_current);
    if (dir.cdUp())
        navigateTo(dir.absolutePath());
}

void MainWindow::openIndex(const QModelIndex &index)
{
    const QString path = m_model->filePath(index);
    if (m_model->isDir(index))
        navigateTo(path);
    else
        QDesktopServices::openUrl(QUrl::fromLocalFile(path));
}

void MainWindow::updateActions()
{
    m_backAct->setEnabled(!m_backHistory.isEmpty());
    m_forwardAct->setEnabled(!m_forwardHistory.isEmpty());
    m_upAct->setEnabled(!m_current.isEmpty() && !QDir(m_current).isRoot());
    m_backButton->setEnabled(m_backAct->isEnabled());
    m_forwardButton->setEnabled(m_forwardAct->isEnabled());
}

// ----------------------------------------------------------- view modes ---

void MainWindow::setGridMode(bool grid)
{
    m_grid = grid;
    m_delegate->setGridMode(grid);
    if (grid) {
        m_view->setViewMode(QListView::IconMode);
        m_view->setMovement(QListView::Static);
        m_view->setGridSize(m_delegate->gridCellSize(m_view->fontMetrics()));
        m_view->setSpacing(0);
    } else {
        m_view->setViewMode(QListView::ListMode);
        m_view->setGridSize(QSize());
        m_view->setSpacing(0);
    }
    m_view->setResizeMode(QListView::Adjust);
    m_view->setWordWrap(true);
    m_viewButton->setIcon(glyphIcon(grid ? Glyph::List : Glyph::Grid, m_theme.fg));
    m_viewButton->setToolTip(grid ? tr("List view (Ctrl+2)") : tr("Grid view (Ctrl+1)"));
}

void MainWindow::setShowHidden(bool show)
{
    m_showHidden = show;
    updateFilter();
    updateFloatingBar();
}

void MainWindow::setSearchActive(bool active)
{
    if (!m_locationStack)
        return;
    if (m_searchAct->isChecked() != active)
        m_searchAct->setChecked(active); // re-enters with the same value
    if (active) {
        m_locationStack->setCurrentWidget(m_searchEdit);
        m_searchEdit->setFocus(Qt::ShortcutFocusReason);
        m_searchEdit->selectAll();
    } else if (m_locationStack->currentWidget() != m_pathBar) {
        m_searchEdit->clear();
        m_locationStack->setCurrentWidget(m_pathBar);
        m_view->setFocus();
    }
}

// Search = filter the current folder by name (case-insensitive substring).
void MainWindow::updateFilter()
{
    QDir::Filters filters = QDir::AllEntries | QDir::NoDotAndDotDot;
    const QString query = m_searchEdit ? m_searchEdit->text().trimmed() : QString();
    if (query.isEmpty()) {
        filters |= QDir::AllDirs;
        m_model->setNameFilters({});
    } else {
        m_model->setNameFilters({'*' + query + '*'});
    }
    if (m_showHidden)
        filters |= QDir::Hidden;
    m_model->setFilter(filters);
}

// -------------------------------------------------------- floating bar ---

void MainWindow::showMessage(const QString &text)
{
    m_message = text;
    m_messageTimer->start();
    updateFloatingBar();
}

void MainWindow::updateFloatingBar()
{
    QString text = m_message;
    if (text.isEmpty()) {
        const QModelIndexList selected = m_view->selectionModel()->selectedIndexes();
        if (selected.size() == 1) {
            const QModelIndex &idx = selected.first();
            text = m_model->isDir(idx)
                       ? tr("“%1” selected").arg(m_model->fileName(idx))
                       : tr("“%1” selected (%2)")
                             .arg(m_model->fileName(idx),
                                  QLocale().formattedDataSize(m_model->size(idx)));
        } else if (selected.size() > 1) {
            qint64 bytes = 0;
            for (const QModelIndex &idx : selected)
                if (!m_model->isDir(idx))
                    bytes += m_model->size(idx);
            text = tr("%n items selected", nullptr, int(selected.size()));
            if (bytes > 0)
                text += QStringLiteral(" (%1)").arg(QLocale().formattedDataSize(bytes));
        }
    }

    if (text.isEmpty()) {
        m_floatingBar->hide();
        return;
    }
    m_floatingBar->setText(text);
    m_floatingBar->adjustSize();
    positionFloatingBar();
    m_floatingBar->show();
    m_floatingBar->raise();
}

void MainWindow::positionFloatingBar()
{
    const int margin = 10;
    const int maxWidth = qMax(100, m_view->width() - 2 * margin);
    if (m_floatingBar->width() > maxWidth)
        m_floatingBar->resize(maxWidth, m_floatingBar->height());
    m_floatingBar->move(m_view->width() - m_floatingBar->width() - margin,
                        m_view->height() - m_floatingBar->height() - margin);
}

// ------------------------------------------------- window / decorations ---

void MainWindow::updateWindowState()
{
    const bool framed = m_csd && !isMaximized() && !isFullScreen();
    if (m_root && m_root->layout())
        m_root->layout()->setContentsMargins(framed ? QMargins(1, 1, 1, 1) : QMargins());
    if (m_maxButton) {
        m_maxButton->setIcon(
            glyphIcon(isMaximized() ? Glyph::Restore : Glyph::Maximize, m_theme.fg));
        m_maxButton->setToolTip(isMaximized() ? tr("Restore") : tr("Maximize"));
    }
}

Qt::Edges MainWindow::edgesAt(const QPoint &pos) const
{
    if (!m_csd || isMaximized() || isFullScreen())
        return {};
    Qt::Edges edges;
    if (pos.x() < ResizeMargin)
        edges |= Qt::LeftEdge;
    else if (pos.x() >= width() - ResizeMargin)
        edges |= Qt::RightEdge;
    if (pos.y() < ResizeMargin)
        edges |= Qt::TopEdge;
    else if (pos.y() >= height() - ResizeMargin)
        edges |= Qt::BottomEdge;
    return edges;
}

void MainWindow::updateResizeCursor(Qt::Edges edges)
{
    if (!edges) {
        if (m_resizeCursor) {
            QGuiApplication::restoreOverrideCursor();
            m_resizeCursor = false;
        }
        return;
    }
    Qt::CursorShape shape = Qt::SizeHorCursor;
    if (edges == (Qt::LeftEdge | Qt::TopEdge) || edges == (Qt::RightEdge | Qt::BottomEdge))
        shape = Qt::SizeFDiagCursor;
    else if (edges == (Qt::RightEdge | Qt::TopEdge) || edges == (Qt::LeftEdge | Qt::BottomEdge))
        shape = Qt::SizeBDiagCursor;
    else if (edges & (Qt::TopEdge | Qt::BottomEdge))
        shape = Qt::SizeVerCursor;

    if (m_resizeCursor)
        QGuiApplication::changeOverrideCursor(shape);
    else
        QGuiApplication::setOverrideCursor(shape);
    m_resizeCursor = true;
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event)
{
    if (m_csd && watched == windowHandle()) {
        switch (event->type()) {
        case QEvent::MouseMove: {
            auto *me = static_cast<QMouseEvent *>(event);
            if (me->buttons() == Qt::NoButton)
                updateResizeCursor(edgesAt(me->position().toPoint()));
            break;
        }
        case QEvent::MouseButtonPress: {
            auto *me = static_cast<QMouseEvent *>(event);
            const Qt::Edges edges = edgesAt(me->position().toPoint());
            if (me->button() == Qt::LeftButton && edges) {
                windowHandle()->startSystemResize(edges);
                return true;
            }
            break;
        }
        case QEvent::Leave:
            updateResizeCursor({});
            break;
        default:
            break;
        }
    } else if (watched == m_view && event->type() == QEvent::Resize) {
        if (m_floatingBar->isVisible())
            positionFloatingBar();
        if (m_opsPanel->isVisible()) {
            m_opsPanel->reposition();
            m_toast->setAvoidRect(m_opsPanel->geometry());
        }
        if (m_toast->isVisible())
            m_toast->reposition();
    } else if (watched == m_searchEdit && event->type() == QEvent::KeyPress) {
        if (static_cast<QKeyEvent *>(event)->key() == Qt::Key_Escape) {
            setSearchActive(false);
            return true;
        }
    }
    return QMainWindow::eventFilter(watched, event);
}

// ------------------------------------------------------ file operations ---

QStringList MainWindow::selectedPaths() const
{
    QStringList paths;
    for (const QModelIndex &index : m_view->selectionModel()->selectedIndexes())
        paths.append(m_model->filePath(index));
    return paths;
}

void MainWindow::openSelection()
{
    const QModelIndexList selected = m_view->selectionModel()->selectedIndexes();
    if (selected.size() == 1) {
        openIndex(selected.first());
        return;
    }
    for (const QModelIndex &index : selected) {
        if (!m_model->isDir(index))
            QDesktopServices::openUrl(QUrl::fromLocalFile(m_model->filePath(index)));
    }
}

void MainWindow::copySelection(bool cut)
{
    const QStringList paths = selectedPaths();
    if (paths.isEmpty())
        return;
    FileOperations::setClipboardFiles(paths, cut);
    const int n = int(paths.size());
    if (cut)
        showMessage(n == 1 ? tr("“%1” will be moved when you paste").arg(QFileInfo(paths.first()).fileName())
                           : tr("%1 items will be moved when you paste").arg(n));
    else
        showMessage(n == 1 ? tr("“%1” copied").arg(QFileInfo(paths.first()).fileName())
                           : tr("%1 items copied").arg(n));
}

void MainWindow::paste()
{
    QStringList paths;
    bool cut = false;
    if (!FileOperations::clipboardFiles(&paths, &cut)) {
        showMessage(tr("There is nothing to paste"));
        return;
    }
    if (cut) {
        m_ops->move(paths, m_current);
        QApplication::clipboard()->clear(); // a cut is pasted only once
    } else {
        m_ops->copy(paths, m_current);
    }
}

void MainWindow::trashSelection()
{
    const QStringList paths = selectedPaths();
    if (!paths.isEmpty())
        m_ops->trash(paths);
}

void MainWindow::updateEditActions()
{
    const bool hasSelection = m_view && m_view->selectionModel()->hasSelection();
    for (QAction *action : {m_openAct, m_cutAct, m_copyAct, m_trashAct})
        action->setEnabled(hasSelection);
    m_pasteAct->setEnabled(FileOperations::clipboardHasFiles());
    m_undoAct->setEnabled(m_ops->canUndo());
    m_undoAct->setText(m_ops->undoText());
}

void MainWindow::showContextMenu(const QPoint &pos)
{
    const QModelIndex index = m_view->indexAt(pos);
    QItemSelectionModel *selection = m_view->selectionModel();
    if (index.isValid() && !selection->isSelected(index))
        selection->setCurrentIndex(index, QItemSelectionModel::ClearAndSelect);
    else if (!index.isValid())
        selection->clearSelection();
    updateEditActions();

    QMenu *menu = makeMenu();
    menu->setAttribute(Qt::WA_DeleteOnClose);
    if (index.isValid()) {
        menu->addAction(m_openAct);
        menu->addSeparator();
        menu->addAction(m_cutAct);
        menu->addAction(m_copyAct);
        menu->addSeparator();
        menu->addAction(m_trashAct);
    } else {
        menu->addAction(m_pasteAct);
        menu->addAction(m_undoAct);
        menu->addSeparator();
        menu->addAction(m_copyLocationAct);
        menu->addAction(m_hiddenAct);
    }
    menu->popup(m_view->viewport()->mapToGlobal(pos));
}

void MainWindow::onJobFinished(FileJob *job)
{
    const QList<TransferResult> results = job->results();
    const QStringList errors = job->errors();
    const int done = int(results.size());
    const bool userAction = job->property(FileOperations::UserActionProperty).toBool();

    if (job->isCanceled()) {
        showMessage(tr("Canceled"));
    } else if (!userAction) {
        if (done > 0)
            showMessage(tr("Undone"));
    } else if (job->kind() == FileJob::Kind::Trash && done > 0) {
        const QString text =
            done == 1 ? tr("“%1” moved to the Trash").arg(QFileInfo(results.first().source).fileName())
                      : tr("%n items moved to the Trash", nullptr, done);
        m_toast->showMessage(text, tr("Undo"), [this] { m_ops->undo(); });
    } else if (done > 0) {
        const bool copied = job->kind() == FileJob::Kind::Copy;
        if (done == 1)
            showMessage(copied ? tr("1 item copied") : tr("1 item moved"));
        else
            showMessage(copied ? tr("%1 items copied").arg(done) : tr("%1 items moved").arg(done));
    }

    if (!errors.isEmpty()) {
        QString text = errors.mid(0, 10).join('\n');
        if (errors.size() > 10)
            text += '\n' + tr("…and %n more", nullptr, int(errors.size() - 10));
        QMessageBox::warning(this, tr("Some items couldn't be processed"), text);
    }
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    if (!m_ops->activeJobs().isEmpty()) {
        QMessageBox box(QMessageBox::Question, tr("Cancel file operations?"),
                        tr("Files are still being copied or moved. Closing now will cancel "
                           "them; items already finished are kept."),
                        QMessageBox::NoButton, this);
        QPushButton *keep = box.addButton(tr("Keep Working"), QMessageBox::RejectRole);
        box.addButton(tr("Cancel and Close"), QMessageBox::AcceptRole);
        box.setDefaultButton(keep);
        box.exec();
        if (box.clickedButton() == keep) {
            event->ignore();
            return;
        }
        m_ops->cancelAll();
    }
    QMainWindow::closeEvent(event);
}
