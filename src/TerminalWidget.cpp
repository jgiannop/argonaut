#include "TerminalWidget.h"

#include "Pty.h"

#include <QApplication>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QMenu>
#include <QPainter>
#include <QProcessEnvironment>
#include <QScrollBar>
#include <QTimer>
#include <QUrl>

namespace {

constexpr int MaxScrollback = 10000;
constexpr int Padding = 6;

// GNOME Console's palette: readable on dark and light backgrounds.
const char *const kAnsi[16] = {
    "#241f31", "#c01c28", "#2ec27e", "#f5c211", "#1e78e4", "#9841bb", "#0ab9dc", "#c0bfbc",
    "#5e5c64", "#ed333b", "#57e389", "#f8e45c", "#51a1ff", "#c061cb", "#4fd2fd", "#f6f5f4",
};

VTermColor rgb(const QColor &c)
{
    VTermColor color;
    vterm_color_rgb(&color, uint8_t(c.red()), uint8_t(c.green()), uint8_t(c.blue()));
    return color;
}

QColor opaque(const QColor &over, const QColor &under)
{
    const qreal a = over.alphaF();
    return QColor::fromRgbF(over.redF() * a + under.redF() * (1 - a),
                            over.greenF() * a + under.greenF() * (1 - a),
                            over.blueF() * a + under.blueF() * (1 - a));
}

QString cellString(const VTermScreenCell &cell)
{
    QString text;
    for (int i = 0; i < VTERM_MAX_CHARS_PER_CELL && cell.chars[i]; ++i)
        text += QString::fromUcs4(reinterpret_cast<const char32_t *>(&cell.chars[i]), 1);
    return text;
}

VTermScreenCell blankCell()
{
    VTermScreenCell cell{};
    cell.width = 1;
    vterm_color_indexed(&cell.fg, 7);
    cell.fg.type = VTERM_COLOR_DEFAULT_FG;
    cell.bg.type = VTERM_COLOR_DEFAULT_BG;
    return cell;
}

// A folder a shell says it is in, as the rest of the program writes it; empty
// when it isn't one.
QString folderNamed(const QString &path)
{
    if (path.isEmpty())
        return {};
    const QFileInfo info(QDir::fromNativeSeparators(path));
    return info.isAbsolute() && info.isDir() ? info.canonicalFilePath() : QString();
}

} // namespace

TerminalWidget::TerminalWidget(QWidget *parent)
    : QWidget(parent)
{
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_InputMethodEnabled);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setCursor(Qt::IBeamCursor);

    m_font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
#if defined(Q_OS_WIN)
    // Windows' "fixed font" is Courier New; its terminals use these.
    for (const char *family : {"Cascadia Mono", "Consolas"}) {
        if (QFontDatabase::hasFamily(QString::fromLatin1(family))) {
            m_font = QFont(QString::fromLatin1(family));
            break;
        }
    }
#endif
    m_font.setStyleHint(QFont::Monospace);
    if (m_font.pointSizeF() < 10.5)
        m_font.setPointSizeF(10.5);
    const QFontMetrics fm(m_font);
    m_cellWidth = qMax(1, fm.horizontalAdvance(QLatin1Char('M')));
    m_cellHeight = qMax(1, fm.height());
    m_ascent = fm.ascent();

    m_scrollBar = new QScrollBar(Qt::Vertical, this);
    m_scrollBar->setCursor(Qt::ArrowCursor);
    connect(m_scrollBar, &QScrollBar::valueChanged, this, [this](int value) {
        m_scrollOffset = m_scrollBar->maximum() - value;
        update();
    });

    m_vt = vterm_new(m_rows, m_cols);
    vterm_set_utf8(m_vt, 1);
    vterm_output_set_callback(m_vt, &TerminalWidget::onOutput, this);
    m_screen = vterm_obtain_screen(m_vt);
    static VTermScreenCallbacks callbacks{};
    callbacks.damage = &TerminalWidget::onDamage;
    callbacks.movecursor = &TerminalWidget::onMoveCursor;
    callbacks.settermprop = &TerminalWidget::onTermProp;
    callbacks.bell = &TerminalWidget::onBell;
    callbacks.sb_pushline = &TerminalWidget::onPushLine;
    callbacks.sb_popline = &TerminalWidget::onPopLine;
    vterm_screen_set_callbacks(m_screen, &callbacks, this);
    static VTermStateFallbacks fallbacks{};
    fallbacks.osc = &TerminalWidget::onOsc;
    vterm_screen_set_unrecognised_fallbacks(m_screen, &fallbacks, this);
    vterm_screen_set_damage_merge(m_screen, VTERM_DAMAGE_SCROLL);
    vterm_screen_enable_altscreen(m_screen, 1);
    vterm_screen_reset(m_screen, 1);
    applyColors();
}

TerminalWidget::~TerminalWidget()
{
    delete m_pty; // ends the shell; nothing more comes for libvterm
    m_pty = nullptr;
    vterm_free(m_vt);
}

// ---------------------------------------------------------------- shell ---

bool TerminalWidget::start(const QString &directory)
{
    if (isRunning())
        return true;
    // A fresh screen for a new session.
    m_scrollback.clear();
    m_scrollOffset = 0;
    m_hasSelection = false;
    m_reportedDirectory.clear();
    vterm_screen_reset(m_screen, 1);
    updateScrollBar();
    update();

    delete m_pty;
    m_pty = new Pty(this);
    connect(m_pty, &Pty::received, this, [this](const QByteArray &bytes) {
        vterm_input_write(m_vt, bytes.constData(), size_t(bytes.size()));
        m_quiet.output();
        vterm_screen_flush_damage(m_screen);
    });
    connect(m_pty, &Pty::finished, this, &TerminalWidget::childExited);
    if (!m_pty->start(directory, m_rows, m_cols)) {
        delete m_pty;
        m_pty = nullptr;
        return false;
    }
    return true;
}

bool TerminalWidget::isRunning() const
{
    return m_pty && m_pty->isRunning();
}

bool TerminalWidget::isShellIdle() const
{
    return isRunning() && m_pty->isIdle();
}

QString TerminalWidget::shellDirectory() const
{
    if (!isRunning())
        return {};
    const QString known = m_pty->currentDirectory();
    return known.isEmpty() ? m_reportedDirectory : known;
}

bool TerminalWidget::changeDirectory(const QString &directory)
{
    if (!isShellIdle())
        return false; // never type into a running program
    const QString where = shellDirectory();
    if (QDir::cleanPath(where) == QDir::cleanPath(directory))
        return true;
    const QByteArray command = m_pty->changeDirectoryCommand(directory);
    if (where.isEmpty()) {
        // Nothing says where the shell is, so nothing could say the line
        // has done its work: it is typed where it can be seen.
        writePty(command);
        return true;
    }
    // The typed line isn't to be seen (QuietCommand.h): the picture waits
    // until the shell has taken it, then the line is taken out.
    m_quietTarget = QFileInfo(directory).canonicalFilePath();
    m_quiet.begin(m_vt);
    setUpdatesEnabled(false);
    if (!m_quietTimer) {
        m_quietTimer = new QTimer(this);
        m_quietTimer->setInterval(15);
        connect(m_quietTimer, &QTimer::timeout, this, [this] {
            const bool done = isShellIdle() && shellDirectory() == m_quietTarget;
            if (m_quiet.settled(done))
                endQuiet();
        });
    }
    m_quietTimer->start();
    writePty(command);
    return true;
}

void TerminalWidget::endQuiet()
{
    if (m_quietTimer)
        m_quietTimer->stop();
    if (!m_quiet.active)
        return;
    if (m_vt)
        m_quiet.finish(m_vt, isShellIdle() && shellDirectory() == m_quietTarget);
    m_quiet.active = false;
    if (m_screen)
        vterm_screen_flush_damage(m_screen);
    setUpdatesEnabled(true);
    update();
}

void TerminalWidget::writePty(const QByteArray &bytes)
{
    if (isRunning())
        m_pty->write(bytes);
}

void TerminalWidget::childExited()
{
    endQuiet();
    m_pty->deleteLater(); // we are inside its signal
    m_pty = nullptr;
    emit finished();
}

// ------------------------------------------------------------- libvterm ---

int TerminalWidget::onDamage(VTermRect, void *user)
{
    static_cast<TerminalWidget *>(user)->update();
    return 1;
}

int TerminalWidget::onMoveCursor(VTermPos pos, VTermPos, int visible, void *user)
{
    auto *self = static_cast<TerminalWidget *>(user);
    self->m_cursor = pos;
    self->m_cursorVisible = visible;
    self->update();
    return 1;
}

int TerminalWidget::onTermProp(VTermProp prop, VTermValue *value, void *user)
{
    auto *self = static_cast<TerminalWidget *>(user);
    switch (prop) {
    case VTERM_PROP_CURSORVISIBLE:
        self->m_cursorVisible = value->boolean;
        self->update();
        break;
    case VTERM_PROP_ALTSCREEN:
        self->m_altScreen = value->boolean;
        self->updateScrollBar();
        break;
    case VTERM_PROP_MOUSE:
        self->m_mouseMode = value->number;
        if (self->m_mouseMode == VTERM_PROP_MOUSE_NONE)
            self->m_mouseDown = false;
        break;
    case VTERM_PROP_TITLE:
        if (value->string.initial)
            self->m_titleBuffer.clear();
        self->m_titleBuffer.append(value->string.str, qsizetype(value->string.len));
        if (value->string.final) {
            self->m_title = QString::fromUtf8(self->m_titleBuffer);
#if defined(Q_OS_WIN)
            // Our prompts make the title the shell's folder (PtyWindows.cpp),
            // and ConPTY passes a title on whatever else it does with OSCs.
            const QString folder = folderNamed(self->m_title);
            if (!folder.isEmpty())
                self->m_reportedDirectory = folder;
#endif
            emit self->titleChanged(self->m_title);
        }
        break;
    default:
        break;
    }
    return 1;
}

int TerminalWidget::onBell(void *)
{
    return 1;
}

// What the shell says about where it is, where the system can't tell us
// (Windows, macOS): OSC 7 (file://host/path, as VTE's shells and zsh on
// macOS send it) and OSC 9;9 (path; Windows Terminal's, which our PowerShell
// and cmd prompts send: PtyWindows.cpp).
int TerminalWidget::onOsc(int command, VTermStringFragment frag, void *user)
{
    auto *self = static_cast<TerminalWidget *>(user);
    if (command != 7 && command != 9)
        return 0;
    if (frag.initial)
        self->m_oscBuffer.clear();
    if (self->m_oscBuffer.size() < 8192)
        self->m_oscBuffer.append(frag.str, qsizetype(frag.len));
    if (!frag.final)
        return 1;
    const QString text = QString::fromUtf8(self->m_oscBuffer);
    self->m_oscBuffer.clear();
    QString path;
    if (command == 7) {
        path = QUrl(text).toLocalFile();
    } else if (text.startsWith(QLatin1String("9;"))) {
        path = text.mid(2);
        if (path.size() >= 2 && path.startsWith(QLatin1Char('"')) && path.endsWith(QLatin1Char('"')))
            path = path.mid(1, path.size() - 2);
    } else {
        return 0; // another OSC 9 (a notification): not ours
    }
    const QString folder = folderNamed(path);
    if (!folder.isEmpty())
        self->m_reportedDirectory = folder;
    return 1;
}

int TerminalWidget::onPushLine(int cols, const VTermScreenCell *cells, void *user)
{
    auto *self = static_cast<TerminalWidget *>(user);
    self->m_quiet.linePushed();
    self->m_scrollback.append(Cells(cells, cells + cols));
    if (self->m_scrollback.size() > MaxScrollback) {
        self->m_scrollback.removeFirst();
        self->m_selAnchor.ry() -= 1;
        self->m_selEnd.ry() -= 1;
    } else if (self->m_scrollOffset > 0) {
        ++self->m_scrollOffset; // keep the view still while reading history
    }
    self->updateScrollBar();
    return 1;
}

int TerminalWidget::onPopLine(int cols, VTermScreenCell *cells, void *user)
{
    auto *self = static_cast<TerminalWidget *>(user);
    if (self->m_scrollback.isEmpty())
        return 0;
    const Cells line = self->m_scrollback.takeLast();
    for (int i = 0; i < cols; ++i)
        cells[i] = i < line.size() ? line.at(i) : blankCell();
    self->m_scrollOffset = qMin(self->m_scrollOffset, self->scrollbackSize());
    self->updateScrollBar();
    return 1;
}

void TerminalWidget::onOutput(const char *bytes, size_t length, void *user)
{
    static_cast<TerminalWidget *>(user)->writePty(QByteArray(bytes, qsizetype(length)));
}

// ------------------------------------------------------- size & colours ---

void TerminalWidget::setTheme(const Theme &theme)
{
    m_theme = theme;
    applyColors();
    update();
}

void TerminalWidget::applyColors()
{
    m_background = m_theme.window.isValid() ? (m_theme.dark ? m_theme.window.darker(112)
                                                             : m_theme.window.lighter(102))
                                            : QColor("#1e1e1e");
    m_foreground = m_theme.fg.isValid() ? opaque(m_theme.fg, m_background) : QColor("#e0e0e0");
    VTermState *state = vterm_obtain_state(m_vt);
    const VTermColor fg = rgb(m_foreground);
    const VTermColor bg = rgb(m_background);
    vterm_state_set_default_colors(state, &fg, &bg);
    for (int i = 0; i < 16; ++i) {
        VTermColor c = rgb(QColor(kAnsi[i]));
        vterm_state_set_palette_color(state, i, &c);
    }
}

void TerminalWidget::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    const int barWidth = m_scrollBar->sizeHint().width();
    m_scrollBar->setGeometry(width() - barWidth, 0, barWidth, height());
    updateGridSize();
}

void TerminalWidget::updateGridSize()
{
    const int usableWidth = width() - 2 * Padding - m_scrollBar->width();
    const int rows = qMax(2, (height() - 2 * Padding) / m_cellHeight);
    const int cols = qMax(10, usableWidth / m_cellWidth);
    if (rows == m_rows && cols == m_cols)
        return;
    m_rows = rows;
    m_cols = cols;
    vterm_set_size(m_vt, m_rows, m_cols);
    vterm_screen_flush_damage(m_screen);
    if (isRunning())
        m_pty->resize(m_rows, m_cols);
    updateScrollBar();
    update();
}

void TerminalWidget::updateScrollBar()
{
    const int lines = m_altScreen ? 0 : scrollbackSize();
    const QSignalBlocker block(m_scrollBar);
    m_scrollBar->setRange(0, lines);
    m_scrollBar->setPageStep(m_rows);
    m_scrollOffset = qBound(0, m_scrollOffset, lines);
    m_scrollBar->setValue(lines - m_scrollOffset);
    m_scrollBar->setVisible(lines > 0);
}

void TerminalWidget::scrollToBottom()
{
    if (m_scrollOffset == 0)
        return;
    m_scrollOffset = 0;
    updateScrollBar();
    update();
}

void TerminalWidget::setPassthroughShortcuts(const QList<QKeySequence> &shortcuts)
{
    m_passthrough = shortcuts;
}

// -------------------------------------------------------------- drawing ---

bool TerminalWidget::cellAt(int line, int col, VTermScreenCell *cell) const
{
    const int sb = scrollbackSize();
    if (line < sb) {
        const Cells &cells = m_scrollback.at(line);
        if (col < cells.size()) {
            *cell = cells.at(col);
            return true;
        }
        *cell = blankCell();
        return true;
    }
    const int row = line - sb;
    if (row < 0 || row >= m_rows || col < 0 || col >= m_cols)
        return false;
    return vterm_screen_get_cell(m_screen, VTermPos{row, col}, cell) != 0;
}

QColor TerminalWidget::colorOf(VTermColor color, bool foreground) const
{
    if (foreground ? VTERM_COLOR_IS_DEFAULT_FG(&color) : VTERM_COLOR_IS_DEFAULT_BG(&color))
        return foreground ? m_foreground : m_background;
    vterm_screen_convert_color_to_rgb(m_screen, &color);
    return QColor(color.rgb.red, color.rgb.green, color.rgb.blue);
}

bool TerminalWidget::isSelected(int line, int col) const
{
    if (!m_hasSelection)
        return false;
    QPoint a = m_selAnchor;
    QPoint b = m_selEnd;
    if (a.y() > b.y() || (a.y() == b.y() && a.x() > b.x()))
        std::swap(a, b);
    if (line < a.y() || line > b.y())
        return false;
    if (line == a.y() && col < a.x())
        return false;
    if (line == b.y() && col > b.x())
        return false;
    return true;
}

void TerminalWidget::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.fillRect(rect(), m_background);
    p.setFont(m_font);

    QFont bold = m_font;
    bold.setBold(true);
    QFont italic = m_font;
    italic.setItalic(true);
    const QColor selection = m_theme.itemSelected.isValid()
                                 ? opaque(m_theme.itemSelected, m_background)
                                 : QColor("#3d5a80");
    const int top = scrollbackSize() - m_scrollOffset; // absolute line at row 0

    for (int row = 0; row < m_rows; ++row) {
        const int line = top + row;
        const int y = Padding + row * m_cellHeight;
        for (int col = 0; col < m_cols;) {
            VTermScreenCell cell;
            if (!cellAt(line, col, &cell)) {
                ++col;
                continue;
            }
            const int span = cell.width == 2 ? 2 : 1;
            QColor fg = colorOf(cell.fg, true);
            QColor bg = colorOf(cell.bg, false);
            if (cell.attrs.reverse)
                std::swap(fg, bg);
            if (isSelected(line, col))
                bg = selection;
            const QRect box(Padding + col * m_cellWidth, y, span * m_cellWidth, m_cellHeight);
            if (bg != m_background)
                p.fillRect(box, bg);

            const QString text = cell.attrs.conceal ? QString() : cellString(cell);
            if (!text.isEmpty() && text != QLatin1String(" ")) {
                p.setFont(cell.attrs.bold ? bold : (cell.attrs.italic ? italic : m_font));
                p.setPen(fg);
                p.drawText(box.left(), y + m_ascent, text);
            }
            if (cell.attrs.underline) {
                p.setPen(fg);
                p.drawLine(box.left(), y + m_ascent + 2, box.right(), y + m_ascent + 2);
            }
            if (cell.attrs.strike) {
                p.setPen(fg);
                p.drawLine(box.left(), y + m_cellHeight / 2, box.right(), y + m_cellHeight / 2);
            }
            col += span;
        }
    }

    // Cursor: a block when focused, an outline otherwise.
    if (m_cursorVisible && m_scrollOffset == 0 && isRunning()) {
        const QRect box(Padding + m_cursor.col * m_cellWidth, Padding + m_cursor.row * m_cellHeight,
                        m_cellWidth, m_cellHeight);
        if (hasFocus()) {
            p.fillRect(box, m_foreground);
            VTermScreenCell cell;
            if (vterm_screen_get_cell(m_screen, m_cursor, &cell)) {
                const QString text = cellString(cell);
                if (!text.isEmpty()) {
                    p.setFont(cell.attrs.bold ? bold : m_font);
                    p.setPen(m_background);
                    p.drawText(box.left(), box.top() + m_ascent, text);
                }
            }
        } else {
            p.setPen(m_foreground);
            p.drawRect(box.adjusted(0, 0, -1, -1));
        }
    }
}

// ------------------------------------------------------------- keyboard ---

bool TerminalWidget::event(QEvent *event)
{
    // Claim every key (Ctrl+C, Delete, Ctrl+A…) before the window's
    // shortcuts see it, except the few that must keep working here.
    if (event->type() == QEvent::ShortcutOverride) {
        auto *key = static_cast<QKeyEvent *>(event);
        const QKeySequence pressed(key->keyCombination());
        for (const QKeySequence &passthrough : std::as_const(m_passthrough)) {
            if (passthrough.matches(pressed) == QKeySequence::ExactMatch)
                return QWidget::event(event);
        }
        event->accept();
        return true;
    }
    return QWidget::event(event);
}

void TerminalWidget::keyPressEvent(QKeyEvent *event)
{
    const Qt::KeyboardModifiers mods = event->modifiers();
    const int key = event->key();

    // Terminal-style clipboard keys.
    if (mods == (Qt::ControlModifier | Qt::ShiftModifier)) {
        if (key == Qt::Key_C) {
            copySelection();
            return;
        }
        if (key == Qt::Key_V) {
            pasteText(QGuiApplication::clipboard()->text());
            return;
        }
    }
    // Ctrl+Up: the file pane goes to where the shell is. Ours, not the
    // shell's: nothing is sent to it.
    if (mods == Qt::ControlModifier && key == Qt::Key_Up) {
        const QString directory = shellDirectory();
        if (!directory.isEmpty())
            emit showDirectoryRequested(directory);
        return;
    }
    if (mods == Qt::ShiftModifier && key == Qt::Key_Insert) {
        pasteText(QGuiApplication::clipboard()->text());
        return;
    }
    if (mods == Qt::ShiftModifier && (key == Qt::Key_PageUp || key == Qt::Key_PageDown)) {
        m_scrollBar->setValue(m_scrollBar->value() + (key == Qt::Key_PageUp ? -1 : 1) * m_rows);
        return;
    }

    if (!isRunning())
        return;
    scrollToBottom();
    m_hasSelection = false;

    int vmod = VTERM_MOD_NONE;
    if (mods & Qt::ShiftModifier)
        vmod |= VTERM_MOD_SHIFT;
    if (mods & Qt::AltModifier)
        vmod |= VTERM_MOD_ALT;
    if (mods & Qt::ControlModifier)
        vmod |= VTERM_MOD_CTRL;
    const auto mod = VTermModifier(vmod);

    VTermKey vkey = VTERM_KEY_NONE;
    switch (key) {
    case Qt::Key_Return:
    case Qt::Key_Enter:
        vkey = VTERM_KEY_ENTER;
        break;
    case Qt::Key_Tab:
    case Qt::Key_Backtab:
        vkey = VTERM_KEY_TAB;
        break;
    case Qt::Key_Backspace:
        vkey = VTERM_KEY_BACKSPACE;
        break;
    case Qt::Key_Escape:
        vkey = VTERM_KEY_ESCAPE;
        break;
    case Qt::Key_Up:
        vkey = VTERM_KEY_UP;
        break;
    case Qt::Key_Down:
        vkey = VTERM_KEY_DOWN;
        break;
    case Qt::Key_Left:
        vkey = VTERM_KEY_LEFT;
        break;
    case Qt::Key_Right:
        vkey = VTERM_KEY_RIGHT;
        break;
    case Qt::Key_Insert:
        vkey = VTERM_KEY_INS;
        break;
    case Qt::Key_Delete:
        vkey = VTERM_KEY_DEL;
        break;
    case Qt::Key_Home:
        vkey = VTERM_KEY_HOME;
        break;
    case Qt::Key_End:
        vkey = VTERM_KEY_END;
        break;
    case Qt::Key_PageUp:
        vkey = VTERM_KEY_PAGEUP;
        break;
    case Qt::Key_PageDown:
        vkey = VTERM_KEY_PAGEDOWN;
        break;
    default:
        if (key >= Qt::Key_F1 && key <= Qt::Key_F35)
            vkey = VTermKey(VTERM_KEY_FUNCTION(key - Qt::Key_F1 + 1));
        break;
    }
    if (vkey != VTERM_KEY_NONE) {
        vterm_keyboard_key(m_vt, vkey, key == Qt::Key_Backtab ? VTERM_MOD_SHIFT : mod);
        return;
    }

    // Ctrl+letter etc.: send the base key so libvterm makes the control code.
    if (mods & Qt::ControlModifier) {
        uint32_t c = 0;
        if (key >= Qt::Key_A && key <= Qt::Key_Z)
            c = uint32_t('a' + (key - Qt::Key_A));
        else if (key >= 0x20 && key < 0x7f)
            c = uint32_t(key);
        if (c) {
            vterm_keyboard_unichar(m_vt, c, VTermModifier(vmod & ~VTERM_MOD_SHIFT));
            return;
        }
    }

    const QString text = event->text();
    for (const char32_t c : text.toUcs4()) {
        if (c < 0x20 || c == 0x7f)
            continue; // handled above
        vterm_keyboard_unichar(m_vt, uint32_t(c), VTermModifier(vmod & ~VTERM_MOD_SHIFT));
    }
}

// Composed text from input methods (dead keys, Greek accents, CJK…).
void TerminalWidget::inputMethodEvent(QInputMethodEvent *event)
{
    if (isRunning() && !event->commitString().isEmpty()) {
        scrollToBottom();
        for (const char32_t c : event->commitString().toUcs4())
            vterm_keyboard_unichar(m_vt, uint32_t(c), VTERM_MOD_NONE);
    }
    event->accept();
}

QVariant TerminalWidget::inputMethodQuery(Qt::InputMethodQuery query) const
{
    if (query == Qt::ImCursorRectangle)
        return QRect(Padding + m_cursor.col * m_cellWidth, Padding + m_cursor.row * m_cellHeight,
                     m_cellWidth, m_cellHeight);
    if (query == Qt::ImFont)
        return m_font;
    return QWidget::inputMethodQuery(query);
}

void TerminalWidget::focusInEvent(QFocusEvent *event)
{
    QWidget::focusInEvent(event);
    update();
}

void TerminalWidget::focusOutEvent(QFocusEvent *event)
{
    QWidget::focusOutEvent(event);
    update();
}

// -------------------------------------------------- mouse & clipboard ---

QPoint TerminalWidget::cellUnder(const QPoint &pos) const
{
    const int col = qBound(0, (pos.x() - Padding) / m_cellWidth, m_cols - 1);
    const int row = qBound(0, (pos.y() - Padding) / m_cellHeight, m_rows - 1);
    return QPoint(col, scrollbackSize() - m_scrollOffset + row);
}

namespace {

VTermModifier vtermModifiers(Qt::KeyboardModifiers mods)
{
    int vmod = VTERM_MOD_NONE;
    if (mods & Qt::ShiftModifier)
        vmod |= VTERM_MOD_SHIFT;
    if (mods & Qt::AltModifier)
        vmod |= VTERM_MOD_ALT;
    if (mods & Qt::ControlModifier)
        vmod |= VTERM_MOD_CTRL;
    return VTermModifier(vmod);
}

int vtermButton(Qt::MouseButton button)
{
    switch (button) {
    case Qt::LeftButton: return 1;
    case Qt::MiddleButton: return 2;
    case Qt::RightButton: return 3;
    default: return 0;
    }
}

} // namespace

// The program asked for the mouse, and Shift isn't held (Shift is how text
// is still selected here, as in other terminals).
bool TerminalWidget::sendsMouseTo(Qt::KeyboardModifiers mods) const
{
    return m_vt && isRunning() && m_mouseMode != VTERM_PROP_MOUSE_NONE && !(mods & Qt::ShiftModifier);
}

// libvterm writes the report in the form the program asked for (SGR, …),
// and only what that mode wants (a move without a button only for "any motion").
void TerminalWidget::sendMouse(const QPointF &pos, int button, bool pressed, Qt::KeyboardModifiers mods)
{
    const int col = qBound(0, int((pos.x() - Padding) / m_cellWidth), m_cols - 1);
    const int row = qBound(0, int((pos.y() - Padding) / m_cellHeight), m_rows - 1);
    const VTermModifier mod = vtermModifiers(mods);
    vterm_mouse_move(m_vt, row, col, mod);
    if (button > 0)
        vterm_mouse_button(m_vt, button, pressed, mod);
}

void TerminalWidget::mousePressEvent(QMouseEvent *event)
{
    setFocus(Qt::MouseFocusReason);
    if (sendsMouseTo(event->modifiers()) && vtermButton(event->button()) > 0) {
        scrollToBottom();
        m_hasSelection = false;
        m_mouseDown = true;
        sendMouse(event->position(), vtermButton(event->button()), true, event->modifiers());
        update();
        return;
    }
    if (event->button() == Qt::LeftButton) {
        m_selAnchor = m_selEnd = cellUnder(event->position().toPoint());
        m_selecting = true;
        m_hasSelection = false;
        update();
    } else if (event->button() == Qt::MiddleButton) {
        pasteText(QGuiApplication::clipboard()->text(QClipboard::Selection));
    }
}

void TerminalWidget::mouseMoveEvent(QMouseEvent *event)
{
    if (!m_selecting && sendsMouseTo(event->modifiers())) {
        sendMouse(event->position(), 0, false, event->modifiers());
        return;
    }
    if (!m_selecting)
        return;
    m_selEnd = cellUnder(event->position().toPoint());
    m_hasSelection = m_selEnd != m_selAnchor;
    update();
}

void TerminalWidget::mouseReleaseEvent(QMouseEvent *event)
{
    if (m_mouseDown && vtermButton(event->button()) > 0) {
        if (event->buttons() == Qt::NoButton)
            m_mouseDown = false;
        if (m_vt && m_mouseMode != VTERM_PROP_MOUSE_NONE)
            sendMouse(event->position(), vtermButton(event->button()), false, event->modifiers());
        return;
    }
    if (event->button() != Qt::LeftButton)
        return;
    m_selecting = false;
    if (m_hasSelection && QGuiApplication::clipboard()->supportsSelection())
        QGuiApplication::clipboard()->setText(selectedText(), QClipboard::Selection);
}

// Double-click selects a word (path-friendly: keeps / . - _ ~).
void TerminalWidget::mouseDoubleClickEvent(QMouseEvent *event)
{
    // The second press of a double click: the program's, as the first was.
    if (sendsMouseTo(event->modifiers()) && vtermButton(event->button()) > 0) {
        m_mouseDown = true;
        sendMouse(event->position(), vtermButton(event->button()), true, event->modifiers());
        return;
    }
    const QPoint at = cellUnder(event->position().toPoint());
    auto isWordChar = [this, &at](int col) {
        VTermScreenCell cell;
        if (!cellAt(at.y(), col, &cell))
            return false;
        const QString s = cellString(cell);
        return !s.isEmpty() && (s.at(0).isLetterOrNumber() || QStringLiteral("/._-~:@+").contains(s.at(0)));
    };
    if (!isWordChar(at.x()))
        return;
    int from = at.x();
    int to = at.x();
    while (from > 0 && isWordChar(from - 1))
        --from;
    while (to < m_cols - 1 && isWordChar(to + 1))
        ++to;
    m_selAnchor = QPoint(from, at.y());
    m_selEnd = QPoint(to, at.y());
    m_hasSelection = true;
    m_selecting = false;
    update();
    if (QGuiApplication::clipboard()->supportsSelection())
        QGuiApplication::clipboard()->setText(selectedText(), QClipboard::Selection);
}

void TerminalWidget::wheelEvent(QWheelEvent *event)
{
    event->accept();
    // The program asked for the mouse (Claude Code scrolls its conversation
    // itself) or has the whole screen (less, man): the wheel is the program's,
    // a notch at a time (a touchpad's small steps are added up). Without the
    // mouse it gets arrow keys, three a notch, as GNOME Terminal does.
    const bool toMouse = sendsMouseTo(event->modifiers());
    if (m_vt && isRunning() && (toMouse || m_altScreen)) {
        m_wheelRest += event->angleDelta().y();
        while (qAbs(m_wheelRest) >= 120) {
            const bool up = m_wheelRest > 0;
            m_wheelRest += up ? -120 : 120;
            if (toMouse) {
                sendMouse(event->position(), up ? 4 : 5, true, event->modifiers());
            } else {
                for (int i = 0; i < 3; ++i)
                    vterm_keyboard_key(m_vt, up ? VTERM_KEY_UP : VTERM_KEY_DOWN, VTERM_MOD_NONE);
            }
        }
        return;
    }
    m_wheelRest = 0;
    const int lines = -event->angleDelta().y() / 40; // 3 lines per notch
    if (lines != 0)
        m_scrollBar->setValue(m_scrollBar->value() + lines);
}

QString TerminalWidget::lineText(int line, int fromCol, int toCol) const
{
    QString text;
    for (int col = fromCol; col <= toCol && col < m_cols; ++col) {
        VTermScreenCell cell;
        if (!cellAt(line, col, &cell))
            break;
        const QString s = cellString(cell);
        text += s.isEmpty() ? QStringLiteral(" ") : s;
        if (cell.width == 2)
            ++col;
    }
    while (text.endsWith(QLatin1Char(' ')))
        text.chop(1);
    return text;
}

QString TerminalWidget::selectedText() const
{
    if (!m_hasSelection)
        return {};
    QPoint a = m_selAnchor;
    QPoint b = m_selEnd;
    if (a.y() > b.y() || (a.y() == b.y() && a.x() > b.x()))
        std::swap(a, b);
    QStringList lines;
    for (int line = a.y(); line <= b.y(); ++line)
        lines.append(lineText(line, line == a.y() ? a.x() : 0, line == b.y() ? b.x() : m_cols - 1));
    return lines.join(QLatin1Char('\n'));
}

QString TerminalWidget::screenText() const
{
    QStringList lines;
    for (int row = 0; row < m_rows; ++row)
        lines.append(lineText(scrollbackSize() + row, 0, m_cols - 1));
    return lines.join(QLatin1Char('\n'));
}

void TerminalWidget::copySelection()
{
    if (m_hasSelection)
        QGuiApplication::clipboard()->setText(selectedText());
}

// Paste as one block: with bracketed paste on (bash, zsh, vim…) the shell
// won't run pasted lines until you press Enter.
void TerminalWidget::pasteText(const QString &text)
{
    if (!isRunning() || text.isEmpty())
        return;
    scrollToBottom();
    vterm_keyboard_start_paste(m_vt);
    for (const char32_t c : QString(text).replace(QLatin1String("\r\n"), QLatin1String("\r"))
                                .replace(QLatin1Char('\n'), QLatin1Char('\r'))
                                .toUcs4())
        vterm_keyboard_unichar(m_vt, uint32_t(c), VTERM_MOD_NONE);
    vterm_keyboard_end_paste(m_vt);
}

void TerminalWidget::contextMenuEvent(QContextMenuEvent *event)
{
    if (event->reason() == QContextMenuEvent::Mouse && sendsMouseTo(event->modifiers()))
        return; // the right button went to the program (Shift+right click is ours)
    QMenu menu(this);
    QAction *copy = menu.addAction(tr("Copy"), this, &TerminalWidget::copySelection);
    copy->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_C));
    copy->setEnabled(m_hasSelection);
    QAction *paste = menu.addAction(tr("Paste"), this,
                                    [this] { pasteText(QGuiApplication::clipboard()->text()); });
    paste->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_V));
    menu.exec(event->globalPos());
}
