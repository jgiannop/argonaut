// The menus of the terminal interface (Tui::App): the menu bar of the top
// line, the menu a right click opens, and the list of commands the palette
// is made from. A part of App in a file of its own: App.cpp is long enough.

#include "ClaudeReports.h"
#include "App.h"

#include <QFileInfo>

#include "Frames.h"

#include "ContentType.h"
#include "DirectoryModel.h"
#include "Extensions.h"
#include "FileOperations.h"
#include "Places.h"
#include "Theme.h"

#include <QCoreApplication>
#include <QDir>

namespace Tui {

// The menus of the top line, as they are now: what the window's menu bar
// has, without the tabs. The command palette lists the same things (those
// that can be done now), so a command is written once, here.
QList<App::Menu> App::menus()
{
    auto press = [this](const Key &key) {
        return [this, key] {
            // A command about the files: the keys are the files' again.
            m_inSidebar = false;
            m_inPreview = false;
            normalKey(key);
        };
    };
    auto item = [](const QString &name, const QString &keys, std::function<void()> run, bool enabled = true, const QString &palette = {}) {
        MenuItem i;
        i.name = name;
        i.keys = keys;
        i.run = std::move(run);
        i.enabled = enabled;
        i.palette = palette;
        return i;
    };
    auto line = [] {
        MenuItem i;
        i.separator = true;
        return i;
    };
    // An item that is on or off: ticked in the menu, "Show…" or "Hide…" in the palette.
    auto ticked = [&item](const QString &name, bool on, const QString &keys, std::function<void()> run, const QString &whenOn, const QString &whenOff) {
        MenuItem i = item(name, keys, std::move(run), true, on ? whenOn : whenOff);
        i.mark = on ? QStringLiteral("✓") : QString();
        return i;
    };
    // What extensions put in the window's menus (System Dashboard in View).
    auto fromExtensions = [this, &line](QList<MenuItem> *items, const char *menu) {
        const QList<Extensions::Command> added = Extensions::Host::instance()->menuCommands(QString::fromLatin1(menu));
        if (!added.isEmpty())
            items->append(line());
        for (const Extensions::Command &command : added) {
            MenuItem i;
            i.name = command.label;
            i.palette = Extensions::paletteName(command.label); // "ext::…" in the palette
            i.run = [this, command] { runExtensionCommand(command); };
            i.late = true;
            items->append(i);
        }
    };
    const Pane &p = pane();
    const bool some = !targets().isEmpty();
    QList<Menu> list;

    Menu file{QStringLiteral("File"), U'f', {}};
    file.items.append(item(QStringLiteral("New Folder…"), QStringLiteral("F7"), press(Key::of(Key::F7))));
    file.items.append(item(QStringLiteral("Open"), QStringLiteral("Enter"), press(Key::of(Key::Enter)), some));
    file.items.append(item(QStringLiteral("Edit…"), QStringLiteral("Ctrl+E"), press(Key::control('e')),
                           !cursorPath().isEmpty() && QFileInfo(cursorPath()).isFile(), QStringLiteral("Edit the File")));
    file.items.append(item(QStringLiteral("Rename…"), QStringLiteral("F2"), press(Key::of(Key::F2)), some));
    file.items.append(line());
    file.items.append(item(QStringLiteral("Copy to the Other Pane"), QStringLiteral("F5"), press(Key::of(Key::F5)), some && m_split));
    file.items.append(item(QStringLiteral("Move to the Other Pane"), QStringLiteral("F6"), press(Key::of(Key::F6)), some && m_split));
    file.items.append(line());
    file.items.append(item(QStringLiteral("Move to Trash"), QStringLiteral("F8"), press(Key::of(Key::F8)), some));
    file.items.append(item(QStringLiteral("Delete Permanently…"), QStringLiteral("Shift+Del"), press([] { Key k = Key::of(Key::Delete); k.shift = true; return k; }()), some));
    fromExtensions(&file.items, "file");
    file.items.append(line());
    file.items.append(item(QStringLiteral("Quit"), QStringLiteral("Ctrl+Q"), press(Key::control('q')), true, QStringLiteral("Quit Argonaut")));
    file.items.last().late = true;
    list.append(file);

    Menu edit{QStringLiteral("Edit"), U'e', {}};
    edit.items.append(item(m_ops->canUndo() ? m_ops->undoText() : QStringLiteral("Undo"), QStringLiteral("Ctrl+Z"), press(Key::control('z')), m_ops->canUndo()));
    edit.items.append(item(m_ops->canRedo() ? m_ops->redoText() : QStringLiteral("Redo"), QStringLiteral("Ctrl+Y"), press(Key::control('y')), m_ops->canRedo()));
    edit.items.append(line());
    edit.items.append(item(QStringLiteral("Copy"), QStringLiteral("Ctrl+C"), press(Key::control('c')), some));
    edit.items.append(item(QStringLiteral("Cut"), QStringLiteral("Ctrl+X"), press(Key::control('x')), some && canChangeTargets()));
    edit.items.append(item(QStringLiteral("Paste"), QStringLiteral("Ctrl+V"), press(Key::control('v')), FileRules::isTransferTarget(p.current)));
    edit.items.append(line());
    edit.items.append(item(QStringLiteral("Select All"), QStringLiteral("Ctrl+A"), press(Key::control('a'))));
    edit.items.append(item(QStringLiteral("Clear the Selection"), QStringLiteral("Esc"), press(Key::of(Key::Escape)), !p.selected.isEmpty()));
    edit.items.append(line());
    edit.items.append(item(QStringLiteral("Claude API Key…"), QString(), [this] { setClaudeKey(); }));
    fromExtensions(&edit.items, "edit");
    list.append(edit);

    Menu view{QStringLiteral("View"), U'v', {}};
    // The views by name: no going round with F9 to reach one.
    const QPair<View, const char *> views[] = {{View::List, "List"}, {View::Grid, "Grid"}, {View::Icons, "Big Icons"}};
    for (const auto &each : views) {
        const View which = each.first;
        MenuItem i = item(QString::fromLatin1(each.second), QStringLiteral("F9"), [this, which] { setView(which); }, true,
                          QStringLiteral("View as %1").arg(QLatin1String(each.second)));
        i.mark = p.view == which ? QStringLiteral("●") : QString();
        i.inPalette = p.view != which;
        view.items.append(i);
    }
    view.items.append(line());
    view.items.append(ticked(QStringLiteral("Hidden Files"), m_showHidden, QStringLiteral("."), press(Key::character('.')),
                             QStringLiteral("Hide Hidden Files"), QStringLiteral("Show Hidden Files")));
    view.items.append(ticked(QStringLiteral("Preview"), m_preview, QStringLiteral("F3"), press(Key::of(Key::F3)),
                             QStringLiteral("Hide the Preview"), QStringLiteral("Show the Preview")));
    view.items.append(ticked(QStringLiteral("Two Panes"), m_split, QStringLiteral("F4"), press(Key::of(Key::F4)), QStringLiteral("One Pane"),
                             QStringLiteral("Split: Two Panes")));
    view.items.append(ticked(QStringLiteral("Places"), m_sidebar, QStringLiteral("Ctrl+B"), press(Key::control('b')),
                             QStringLiteral("Hide the Places"), QStringLiteral("Show the Places")));
    view.items.append(ticked(QStringLiteral("Terminal"), m_shell != nullptr, QStringLiteral("F12"), [this] { toggleShell(); },
                             QStringLiteral("Go to the Terminal"), QStringLiteral("Open the Terminal")));
    view.items.append(ticked(QStringLiteral("Show .. Row"), m_upRow, QString(), [this] { setUpRowShown(!m_upRow); },
                             QStringLiteral("Hide the .. Row"), QStringLiteral("Show the .. Row")));
    view.items.append(ticked(QStringLiteral("Effects"), m_effects, QString(), [this] { setEffects(!m_effects); },
                             QStringLiteral("Effects Off: The Plain Look"), QStringLiteral("Effects On")));
    view.items.append(line());
    view.items.append(item(QStringLiteral("Read the Folder Again"), QStringLiteral("Ctrl+R"), press(Key::control('r'))));
    MenuItem theme = item(QStringLiteral("Theme…"), QString(), [this] {
        openPalette(); // the themes are a list to choose from by typing
        m_paletteText = QStringLiteral("Theme: ");
        filterPalette();
    });
    theme.inPalette = false;
    view.items.append(theme);
    MenuItem palette = item(QStringLiteral("Command Palette…"), QStringLiteral("Ctrl+P"), [this] { openPalette(); });
    palette.inPalette = false;
    view.items.append(palette);
    fromExtensions(&view.items, "view");
    list.append(view);

    Menu go{QStringLiteral("Go"), U'g', {}};
    Key back = Key::of(Key::Left);
    back.alt = true;
    go.items.append(item(QStringLiteral("Back"), QStringLiteral("Alt+Left"), press(back), !p.back.isEmpty(), QStringLiteral("Go Back")));
    Key forward = Key::of(Key::Right);
    forward.alt = true;
    go.items.append(item(QStringLiteral("Forward"), QStringLiteral("Alt+Right"), press(forward), !p.forward.isEmpty(), QStringLiteral("Go Forward")));
    Key up = Key::of(Key::Up);
    up.alt = true;
    go.items.append(item(QStringLiteral("Up"), QStringLiteral("Alt+Up"), press(up), canGoUp(p), QStringLiteral("Go Up")));
    Key down = Key::of(Key::Down);
    down.alt = true;
    go.items.append(item(QStringLiteral("Down"), QStringLiteral("Alt+Down"), press(down), !downTarget(p).isEmpty(), QStringLiteral("Go Down")));
    go.items.append(item(QStringLiteral("Home"), QStringLiteral("~"), press(Key::character('~')), true, QStringLiteral("Go Home")));
    go.items.append(item(QStringLiteral("Go to Folder…"), QStringLiteral("Ctrl+L"), press(Key::control('l'))));
    // Every answer Claude gives is kept as a page (ClaudeReports): the folder they are in.
    go.items.append(item(QStringLiteral("Claude Reports"), QString(), [this] {
        if (QFileInfo(ClaudeReports::folder()).isDir())
            navigate(pane(), ClaudeReports::folder());
        else
            say(QStringLiteral("No reports yet: every answer Claude gives is kept there"));
    }, true, QStringLiteral("Go to Claude Reports")));
    go.items.append(line());
    // The places, each by its name.
    if (m_places.isEmpty())
        m_places = Places::list();
    for (int i = 0; i < m_places.size(); ++i) {
        const Places::Entry &place = m_places.at(i);
        if (Extensions::Host::isPlacePath(place.path))
            continue;
        MenuItem row = item(place.name, QString(), [this, i] { openPlace(i); }, true, QStringLiteral("Go to %1").arg(place.name));
        row.mark = place.path == p.current ? QStringLiteral("●") : QString();
        row.inPalette = place.path != p.current;
        row.late = true;
        go.items.append(row);
    }
    fromExtensions(&go.items, "go");
    list.append(go);

    Menu help{QStringLiteral("Help"), U'h', {}};
    help.items.append(item(QStringLiteral("All Keys"), QStringLiteral("F1"), press(Key::of(Key::F1)), true, QStringLiteral("Help: All Keys")));
    help.items.append(item(QStringLiteral("About Argonaut"), QString(), [this] {
        const QString version = QCoreApplication::applicationVersion();
        say(QStringLiteral("Argonaut%1 · a file manager · GPL-3.0-or-later · github.com/jgiannop/argonaut")
                .arg(version.isEmpty() ? QString() : QLatin1Char(' ') + version));
    }));
    list.append(help);
    return list;
}

// For the command palette: what the menus have that can be done now, each
// under the name that says it without its menu ("Go Back"), then the themes.
QList<App::Command> App::commands()
{
    QList<Command> list, late;
    const QList<Menu> all = menus();
    // Help and Go first: with nothing typed, the list starts as it always did.
    for (int index : {4, 3, 0, 1, 2}) {
        const Menu &menu = all.at(index);
        for (const MenuItem &item : menu.items) {
            if (item.separator || !item.enabled || !item.inPalette)
                continue;
            (item.late ? late : list).append({item.palette.isEmpty() ? item.name : item.palette, item.keys, item.run});
        }
    }
    // The themes: the terminal's own colours, and every theme the window has.
    if (m_themeKey != QLatin1String("terminal"))
        late.prepend({QStringLiteral("Theme: The Terminal's Own Colours"), QString(), [this] { setTheme(QStringLiteral("terminal")); }});
    QList<Command> themes;
    for (const ThemeChoice &theme : allThemes()) {
        if (theme.key == QLatin1String("system") || theme.key == m_themeKey)
            continue;
        const QString key = theme.key;
        themes.append({QStringLiteral("Theme: %1").arg(theme.name), QString(), [this, key] { setTheme(key); }});
    }
    return list + themes + late;
}

// ---------------------------------------------------------------- menus ---

// The menu a right click opens, at the pointer: what can be done with the
// selection (`onItems`), or with the folder. The items are the menus' own,
// so each is still the same as its key; then what extensions offer for
// exactly these items (Git's Stage, Send by Email), as in the window.
void App::openContextMenu(int x, int y, bool onItems)
{
    const QList<Menu> all = menus();
    Menu menu{QString(), 0, {}};
    auto add = [&](const char *name) {
        const QString wanted = QString::fromUtf8(name);
        for (const Menu &each : all) {
            for (const MenuItem &item : each.items) {
                if (!item.separator && item.name == wanted) {
                    menu.items.append(item);
                    return;
                }
            }
        }
    };
    auto line = [&] {
        MenuItem item;
        item.separator = true;
        menu.items.append(item);
    };
    if (onItems) {
        add("Open");
        add("Edit…");
        add("Rename…");
        line();
        add("Copy");
        add("Cut");
        line();
        add("Copy to the Other Pane");
        add("Move to the Other Pane");
        line();
        add("Move to Trash");
        add("Delete Permanently…");
    } else {
        add("New Folder…");
        add("Paste");
        add("Up");
        line();
        add("Select All");
        line();
        add("Hidden Files");
        add("Read the Folder Again");
    }
    const Pane &p = pane();
    if (!p.model->isTrash()) {
        const QStringList selection = onItems ? targets() : QStringList();
        QList<Extensions::Item> items;
        bool known = true;
        for (const QString &path : selection) {
            const QModelIndex index = p.model->indexForPath(path);
            if (!index.isValid()) {
                known = false;
                break;
            }
            const bool dir = p.model->isDir(index);
            items.append({path, dir ? QStringLiteral("inode/directory") : ContentType::effectiveMime(DirectoryModel::contentResult(index)), dir});
        }
        const QList<Extensions::Command> offered =
            known ? Extensions::Host::instance()->commands(p.model->extensionsFolder(), items) : QList<Extensions::Command>();
        if (!offered.isEmpty())
            line();
        for (const Extensions::Command &command : offered) {
            MenuItem item;
            // No submenus here: a command says which group it is of ("Git: Stage").
            item.name = command.group.isEmpty() ? command.label : QString(command.group).replace(QLatin1Char('/'), QStringLiteral(" › ")) + QStringLiteral(": ") + command.label;
            item.mark = command.checked ? QStringLiteral("✓") : QString();
            item.run = [this, command, selection] { runExtensionCommand(command, selection); };
            menu.items.append(item);
        }
    }
    m_menus = {menu};
    m_menuOpen = 0;
    m_menuCursor = -1;
    m_menuTop = 0;
    m_popup = true;
    m_popupX = x;
    m_popupY = y;
    m_mode = Mode::Menu;
    moveInMenu(1);
}

void App::openMenu(int index)
{
    m_popup = false;
    m_menus = menus();
    m_menuOpen = (index % int(m_menus.size()) + int(m_menus.size())) % int(m_menus.size());
    m_menuCursor = -1;
    m_menuTop = 0;
    m_mode = Mode::Menu;
    moveInMenu(1);
}

void App::closeMenu()
{
    m_menuOpen = -1;
    m_popup = false;
    if (m_mode == Mode::Menu)
        m_mode = Mode::Normal;
    emit changed();
}

// The next item that can be chosen, `step` rows on (round and round).
void App::moveInMenu(int step)
{
    const QList<MenuItem> &items = m_menus.at(m_menuOpen).items;
    const int count = int(items.size());
    int row = m_menuCursor;
    for (int tried = 0; tried < count; ++tried) {
        row = ((row + step) % count + count) % count;
        if (!items.at(row).separator && items.at(row).enabled) {
            m_menuCursor = row;
            break;
        }
    }
    emit changed();
}

void App::runMenuRow(int row)
{
    const QList<MenuItem> &items = m_menus.at(m_menuOpen).items;
    if (row < 0 || row >= items.size() || items.at(row).separator || !items.at(row).enabled)
        return;
    const std::function<void()> run = items.at(row).run;
    closeMenu(); // first: the command may ask something itself
    if (run)
        run();
}

QString App::menuTitle() const
{
    return m_mode == Mode::Menu && m_menuOpen >= 0 && !m_popup ? m_menus.at(m_menuOpen).title : QString();
}

QStringList App::menuNames() const
{
    QStringList names;
    if (m_mode != Mode::Menu || m_menuOpen < 0)
        return names;
    for (const MenuItem &item : m_menus.at(m_menuOpen).items) {
        if (!item.separator)
            names << (item.enabled ? item.name : QStringLiteral("(%1)").arg(item.name));
    }
    return names;
}

void App::menuKey(const Key &key)
{
    switch (key.code) {
    case Key::Escape:
    case Key::F10:
        closeMenu();
        return;
    case Key::Left:
        if (!m_popup) // a right click's menu has no neighbours
            openMenu(m_menuOpen - 1);
        return;
    case Key::Right:
    case Key::Tab:
        if (!m_popup)
            openMenu(m_menuOpen + 1);
        return;
    case Key::Up:
        moveInMenu(-1);
        return;
    case Key::Down:
        moveInMenu(1);
        return;
    case Key::Home:
        m_menuCursor = -1;
        moveInMenu(1);
        return;
    case Key::End:
        m_menuCursor = int(m_menus.at(m_menuOpen).items.size());
        moveInMenu(-1);
        return;
    case Key::Enter:
        runMenuRow(m_menuCursor);
        return;
    case Key::Char:
        if (key.ctrl)
            return;
        // Alt and a menu's letter: that menu. A letter alone: the next item
        // that starts with it.
        if (key.alt) {
            for (int i = 0; i < m_menus.size() && !m_popup; ++i) {
                if (QChar::toLower(key.ch) == m_menus.at(i).letter) {
                    if (i == m_menuOpen)
                        closeMenu();
                    else
                        openMenu(i);
                    return;
                }
            }
            return;
        }
        {
            const QList<MenuItem> &items = m_menus.at(m_menuOpen).items;
            const int count = int(items.size());
            for (int step = 1; step <= count; ++step) {
                const int row = (qMax(0, m_menuCursor) + step) % count;
                const MenuItem &item = items.at(row);
                if (!item.separator && item.enabled && !item.name.isEmpty() && item.name.at(0).toLower() == QChar(QChar::toLower(key.ch))) {
                    m_menuCursor = row;
                    emit changed();
                    return;
                }
            }
        }
        return;
    default:
        return;
    }
}

// The top line's menu titles; where each is goes into m_menuSpots, for the mouse.
void App::drawMenuBar(Screen &screen, int x)
{
    static const char *const titles[] = {"File", "Edit", "View", "Go", "Help"};
    m_menuSpots.clear();
    for (int i = 0; i < int(std::size(titles)); ++i) {
        const QString title = QString::fromLatin1(titles[i]);
        const bool open = m_mode == Mode::Menu && m_menuOpen == i && !m_popup;
        const int width = Screen::widthOf(title) + 2;
        screen.put(x, 0, QLatin1Char(' ') + title + QLatin1Char(' '), {Ink::Normal, open ? Paper::Cursor : Paper::Bar, open});
        m_menuSpots.append({x, x + width, Key()});
        x += width;
    }
}

// The open menu: a box under its title, an item a line, its key at the
// right; what can't be done now is dim.
void App::drawMenu(Screen &screen)
{
    if (m_menuOpen < 0 || m_menuOpen >= m_menus.size() || (!m_popup && m_menuOpen >= m_menuSpots.size()))
        return;
    const QList<MenuItem> &items = m_menus.at(m_menuOpen).items;
    const int count = int(items.size());
    int names = 0, keys = 0;
    for (const MenuItem &item : items) {
        names = qMax(names, Screen::widthOf(item.name));
        keys = qMax(keys, Screen::widthOf(item.keys));
    }
    const int w = screen.width();
    const int h = screen.height();
    const int width = qMin(w, 2 + 2 + names + (keys ? 3 + keys : 0) + 2); // edge, mark, name, key, edge
    const int rows = qMax(1, qMin(count, h - 4));
    // Under its title; a right click's menu just under the pointer, or
    // higher where the screen ends.
    const int x = qBound(0, m_popup ? m_popupX : m_menuSpots.at(m_menuOpen).from, w - width);
    const int top = m_popup ? qBound(1, m_popupY + 1, qMax(1, h - rows - 3)) : 1;
    if (m_menuCursor >= 0 && m_menuCursor < m_menuTop)
        m_menuTop = m_menuCursor;
    if (m_menuCursor >= m_menuTop + rows)
        m_menuTop = m_menuCursor - rows + 1;
    m_menuTop = qBound(0, m_menuTop, qMax(0, count - rows));
    m_menuX = x;
    m_menuY = top + 1;
    m_menuWidth = width;
    m_menuShown = rows;

    const Style box{Ink::Normal, Paper::Bar, false};
    const Style edge{Ink::Accent, Paper::Bar, false};
    const QString rule(width - 2, QChar(0x2500));
    if (m_effects) {
        // The rest steps back (the menu's own titles stay as they are), and
        // the menu throws a shadow to the right and below.
        screen.dim(0, m_popup ? 0 : 1, w, h);
        screen.shade(x + 2, top + rows + 2, width, 1);
        screen.shade(x + width, top + 1, 2, rows + 1);
    }
    screen.fill(x, top, width, rows + 2, box);
    screen.put(x, top, frameTop(width, m_effects), edge);
    screen.put(x, top + 1 + rows, frameBottom(width, m_effects), edge);
    for (int i = 0; i < rows; ++i) {
        const int row = m_menuTop + i;
        const int y = m_menuY + i;
        const MenuItem &item = items.at(row);
        if (item.separator) {
            screen.put(x, y, QStringLiteral("├") + rule + QStringLiteral("┤"), edge);
            continue;
        }
        const bool cursor = row == m_menuCursor;
        const Style style{item.enabled ? Ink::Normal : Ink::Dim, cursor ? Paper::Cursor : Paper::Bar, cursor};
        screen.put(x, y, QStringLiteral("│"), edge);
        screen.put(x + width - 1, y, QStringLiteral("│"), edge);
        screen.fill(x + 1, y, width - 2, 1, style);
        if (!item.mark.isEmpty())
            screen.put(x + 2, y, item.mark, {item.enabled ? Ink::Accent : Ink::Dim, style.paper, false});
        screen.put(x + 4, y, Screen::elided(item.name, width - 6, false), style);
        const int keysWidth = Screen::widthOf(item.keys);
        if (keysWidth > 0 && width - 6 - Screen::widthOf(item.name) > keysWidth)
            screen.put(x + width - 2 - keysWidth, y, item.keys, {cursor && item.enabled ? Ink::Normal : Ink::Dim, style.paper, false});
    }
    // More than fit: the scrollbar's handle on the right edge.
    if (count > rows) {
        const int thumb = qBound(1, rows * rows / count, rows - 1);
        const int at = (rows - thumb) * m_menuTop / (count - rows);
        for (int i = at; i < at + thumb; ++i)
            screen.put(x + width - 1, m_menuY + i, QStringLiteral("█"), edge);
    }
}

} // namespace Tui
