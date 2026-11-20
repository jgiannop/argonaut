#include "StyleSheet.h"

#include "Theme.h"

#include <QColor>

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

QSplitter#sideSplit::handle { background: @border; }
QWidget#sideTree { background: @sidebar; }
QLabel#sideTreeTitle { color: @dim; }
QTreeView#sideTreeView { background: @sidebar; color: @fg; border: none; outline: 0; padding: 0px 6px 6px 6px; }
QTreeView#sideTreeView::item { height: 26px; border: none; border-radius: 6px; }
QTreeView#sideTreeView::item:hover { background: @hover; }
QTreeView#sideTreeView::item:selected { background: @selected; color: @fg; }

QListView#files { background: @window; border: none; outline: 0; padding: 6px 6px; }
QTreeView#detailView { background: @window; border: none; outline: 0; padding: 0px 6px 6px 6px; }
QTreeView#detailView::item, QTreeView#detailView::item:selected,
QTreeView#detailView::branch { background: transparent; }
QTreeView#detailView QHeaderView { background: @window; border: none; }
QTreeView#detailView QHeaderView::section { background: @window; color: @dim; border: none;
    padding: 8px 10px 6px 16px; font-weight: bold; }
QTreeView#detailView QHeaderView::section:hover { color: @fg; }
QTreeView#analyzeView { background: @window; border: none; outline: 0; padding: 0px 6px 6px 6px; }
QTreeView#analyzeView::item { border: none; padding-left: 4px; }
QTreeView#analyzeView::item:hover { background: @hover; }
QTreeView#analyzeView::item:selected { background: @itemSel; color: @fg; }
QTreeView#analyzeView QHeaderView { background: @window; border: none; }
QTreeView#analyzeView QHeaderView::section { background: @window; color: @dim; border: none;
    padding: 8px 10px 6px 16px; font-weight: bold; }
QTreeView#analyzeView QHeaderView::section:hover { color: @fg; }
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
QMenu::item { padding: 7px 32px 7px 28px; border-radius: 6px; }
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

QWidget#menuRow { background: @sidebar; border: none; border-bottom: 1px solid @border; }
QMenuBar#menuBar { background: @sidebar; color: @fg; border: none; padding: 2px 4px; }
QMenuBar#menuBar::item { background: transparent; padding: 4px 10px; border-radius: 6px; }
QMenuBar#menuBar::item:selected { background: @hover; }
QMenuBar#menuBar::item:pressed { background: @pressed; }
QLineEdit#renameEdit { background: @window; color: @fg; border: 2px solid @accent;
    border-radius: 6px; padding: 1px 4px; selection-background-color: @accent;
    selection-color: #ffffff; }

QTabBar#tabs { background: @window; border: none; }
QTabBar#tabs::tab { background: transparent; color: @dim; border: none; border-radius: 6px;
    padding: 5px 8px 5px 14px; margin: 3px 2px; min-width: 120px; }
QTabBar#tabs::tab:selected { background: @pathBg; color: @fg; }
QTabBar#tabs::tab:hover:!selected { background: @hover; }
QTabBar#resultTabs { background: transparent; border: none; }
QTabBar#resultTabs::tab { background: transparent; color: @dim; border: none; border-radius: 6px;
    padding: 4px 6px 4px 10px; margin: 0 2px 2px 0; }
QTabBar#resultTabs::tab:selected { background: @pathBg; color: @fg; }
QTabBar#resultTabs::tab:hover:!selected { background: @hover; }
QTabBar#resultTabs QToolButton { background: @window; border: none; color: @fg; }
QWidget#trashBar { background: @pathBg; border-bottom: 1px solid @border; }
QLabel#trashBarText { color: @fg; }
QToolButton#barButton { background: @button; color: @fg; border: none; border-radius: 6px;
    padding: 5px 14px; font-weight: bold; }
QToolButton#barButton:hover { background: @buttonHover; }
QToolButton#barButton:disabled { color: @dim; }
QWidget#terminalPanel { background: @window; }
QWidget#terminalBar { background: @window; border-top: 1px solid @border; }
QTabBar#terminalTabs { background: transparent; border: none; }
QTabBar#terminalTabs::tab { background: transparent; color: @dim; border: none; border-radius: 6px;
    padding: 3px 6px 3px 10px; margin: 0 2px 0 0; }
QTabBar#terminalTabs::tab:selected { background: @pathBg; color: @fg; }
QTabBar#terminalTabs::tab:hover:!selected { background: @hover; }
QTabBar#terminalTabs QToolButton { background: @window; border: none; color: @fg; }
QSplitter#contentSplitter::handle { background: @border; }

QDialog#conflictDialog { background: @window; }
QLabel#conflictTitle { color: @fg; font-weight: bold; font-size: 15px; }
QLabel#conflictText { color: @dim; }
QCheckBox#conflictCheck { color: @fg; spacing: 8px; }
QCheckBox#conflictCheck::indicator { width: 14px; height: 14px; border: 1px solid @dim;
    border-radius: 4px; background: @pathBg; }
QCheckBox#conflictCheck::indicator:checked { background: @accent; border-color: @accent; }
QWidget#conflictCard { background: @pathBg; border-radius: 10px; }
QLabel#conflictCardHeading { color: @fg; font-weight: bold; }
QLineEdit#conflictName { background: @pathBg; color: @fg; border: none; border-radius: 6px;
    padding: 6px 8px; selection-background-color: @accent; }
QPushButton#conflictButton { background: @button; color: @fg; border: none; border-radius: 6px;
    padding: 7px 14px; }
QPushButton#conflictButton:hover { background: @buttonHover; }
QPushButton#conflictPrimary { background: @accent; color: #ffffff; border: none;
    border-radius: 6px; padding: 7px 16px; font-weight: bold; }

QDialog#propertiesDialog { background: @window; }
QLabel#propLabel { color: @dim; }
QLabel#propValue { color: @fg; }
QLabel#propHeading { color: @fg; font-weight: bold; padding-top: 10px; }
QLabel#propWarning { color: #c88800; font-weight: bold; }
QLabel#propDanger { color: #e01b24; font-weight: bold; }
QComboBox#propCombo { background: @pathBg; color: @fg; border: none; border-radius: 6px;
    padding: 5px 10px; }
QComboBox#propCombo:disabled { color: @dim; }
QComboBox#propCombo::drop-down { border: none; width: 18px; }
QComboBox#propCombo QAbstractItemView { background: @popup; color: @fg; border: 1px solid @border;
    selection-background-color: @hover; selection-color: @fg; outline: 0; }
QDialog#openWithDialog { background: @window; }
QListWidget#appList { background: @pathBg; color: @fg; border-radius: 8px; padding: 4px; outline: 0; }
QListWidget#appList::item { padding: 6px; border-radius: 6px; }
QListWidget#appList::item:selected { background: @itemSel; color: @fg; }
QListWidget#appList::item:hover:!selected { background: @hover; }

QDialog#preferences, QDialog#extensionsDialog { background: @window; }
QFrame#extCard { background: @pathBg; border-radius: 10px; }
QFrame#extCard QLabel#prefHint { background: transparent; }
QLabel#extName { color: @fg; font-weight: bold; background: transparent; }
QLabel#extState { color: @dim; background: transparent; }
QLabel#extState[look="good"] { color: @accent; }
QLabel#extState[look="bad"] { color: #e01b24; }
QScrollArea#prefScroll, QWidget#prefContent { background: @window; }
QDialog#connectDialog { background: @window; }
QLineEdit#connectAddress { background: @pathBg; color: @fg; border: none; border-radius: 6px;
    padding: 7px 9px; selection-background-color: @accent; }
QLineEdit#connectAddress:disabled { color: @dim; }
QLabel#prefHeading { color: @fg; font-weight: bold; font-size: 15px; }
QLabel#prefHint { color: @dim; }
QCheckBox#prefCheck { color: @fg; spacing: 8px; }
QCheckBox#prefCheck::indicator { width: 14px; height: 14px; border: 1px solid @dim;
    border-radius: 4px; background: @pathBg; }
QCheckBox#prefCheck::indicator:checked { background: @accent; border-color: @accent; }
QWidget#quickLook { background: @window; }
QSplitter#previewSplitter::handle { background: @border; }
QToolButton#qlClose { color: @dim; border: none; padding: 2px 6px; border-radius: 6px; }
QToolButton#qlClose:hover { color: @fg; background: @hover; }
QLabel#paneCaption { background: @window; color: @dim; padding: 5px 14px; border-bottom: 1px solid @border; }
QLabel#paneCaption[active="true"] { color: @accent; font-weight: bold; }
QSplitter#paneSplitter::handle { background: @border; }
QLabel#qlTitle { color: @fg; font-weight: bold; font-size: 15px; }
QLabel#qlDetails { color: @dim; }
QScrollArea#qlScroll, QScrollArea#qlScroll > QWidget > QWidget { background: transparent; }
QAbstractScrollArea#qlPdf, QAbstractScrollArea#qlPdf > QWidget { background: transparent; border: none; }
QPlainTextEdit#qlText, QTextBrowser#qlText { background: @pathBg; color: @fg; border: none; border-radius: 8px; padding: 6px; }
QTreeWidget#qlArchive { background: @window; color: @fg; border: none; }
QTreeWidget#qlArchive QHeaderView::section { background: @window; color: @dim; border: none;
    padding: 4px 8px; font-weight: bold; }
QPushButton#prefButton, QPushButton#themesFolderButton, QPushButton#saveThemeButton { background: @pathBg; color: @fg; border: none; border-radius: 6px;
    padding: 7px 16px; }
QPushButton#prefButton:hover, QPushButton#themesFolderButton:hover, QPushButton#saveThemeButton:hover { background: @hover; }
QPushButton#prefButton:disabled, QPushButton#themesFolderButton:disabled, QPushButton#saveThemeButton:disabled { color: @dim; }
QToolButton#themeCard { background: transparent; color: @fg; border: 2px solid transparent;
    border-radius: 12px; padding: 8px 8px 6px 8px; }
QToolButton#themeCard:hover { background: @hover; }
QToolButton#themeCard:checked { border-color: @accent; }

QToolButton#searchOption { background: @pathBg; color: @dim; border: none; border-radius: 8px;
    padding: 0 12px; font-weight: bold; }
QToolButton#searchOption:hover { color: @fg; }
QToolButton#searchOption:checked { background: @accent; color: #ffffff; }
QToolButton#gitBadge { background: transparent; color: @dim; border: none; border-radius: 6px;
    padding: 2px 8px; font-weight: bold; }
QToolButton#gitBadge:hover { background: @hover; }
QFrame#commandPalette { background: @popup; border: 1px solid @border; border-radius: 12px; }
QLineEdit#paletteEdit { background: @pathBg; color: @fg; border: none; border-radius: 8px;
    padding: 8px 10px; font-size: 14px; selection-background-color: @accent; }
QListWidget#paletteList { background: transparent; color: @fg; outline: 0; }
QDialog#helpDialog { background: @window; }
QWidget#helpSide { background: @sidebar; border-right: 1px solid @border; }
QLabel#helpTitle { color: @fg; font-weight: bold; font-size: 15px; padding: 2px 4px; }
QLabel#helpNoMatch { color: @dim; padding: 4px; }
QLineEdit#helpSearch { background: @pathBg; color: @fg; border: none; border-radius: 6px;
    padding: 6px 8px; selection-background-color: @accent; }
QListWidget#helpTopics { background: transparent; color: @fg; outline: 0; }
QListWidget#helpTopics::item { padding: 7px 8px; border-radius: 6px; }
QListWidget#helpTopics::item:selected { background: @selected; color: @fg; }
QListWidget#helpTopics::item:hover:!selected { background: @hover; }
QTextBrowser#helpPage { background: @window; color: @fg; border: none; }

QDialog#aboutDialog { background: @window; }
QLabel#aboutName { color: @fg; font-weight: bold; font-size: 26px; }
QLabel#aboutVersion { background: @itemSel; color: @accent; font-weight: bold; border-radius: 10px;
    padding: 3px 12px; }
QLabel#aboutTagline { color: @dim; font-size: 14px; }
QToolButton#aboutTab { background: transparent; color: @dim; border: none; border-radius: 14px;
    padding: 5px 14px; font-weight: bold; }
QToolButton#aboutTab:hover { background: @hover; color: @fg; }
QToolButton#aboutTab:checked { background: @pathBg; color: @fg; }
QLabel#aboutText { color: @fg; }
QLabel#aboutDim { color: @dim; }
QWidget#aboutCard { background: @pathBg; border-radius: 10px; }
QLabel#aboutCardHeading { color: @fg; font-weight: bold; }
QLabel#aboutCardText { color: @dim; }
QLabel#aboutLabel { color: @dim; }
QWidget#chooserBar { background: @sidebar; border-top: 1px solid @border; }
QLabel#chooserLabel { color: @fg; }
QLabel#aboutValue { color: @fg; }
QPushButton#aboutClose { background: @accent; color: #ffffff; border: none; border-radius: 6px;
    padding: 7px 18px; font-weight: bold; }
)");
    const QList<std::pair<const char *, QColor>> tokens = {
        {"@outline", t.outline},   {"@sidebar", t.sidebar},       {"@window", t.window},
        {"@fg", t.fg},             {"@dim", t.dimFg},             {"@hover", t.hover},
        {"@pressed", t.pressed},   {"@selected", t.selected},     {"@buttonHover", t.buttonHover},
        {"@button", t.button},     {"@pathBg", t.pathBg},         {"@accent", t.accent},
        {"@popup", t.popup},       {"@border", t.border},         {"@scrollHover", t.scrollHover},
        {"@scroll", t.scroll},     {"@splitter", t.dark ? t.sidebar : t.border},
        {"@itemSel", t.itemSelected},
    };
    // Longer tokens first where one is a prefix of another (@button/@buttonHover…).
    for (const auto &[token, color] : tokens)
        css.replace(QLatin1String(token), cssColor(color));
    return css;
}
