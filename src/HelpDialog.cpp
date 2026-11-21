#include "HelpDialog.h"

#include "DirectoryModel.h"
#include "FileJob.h"
#include "Theme.h"

#include <QDesktopServices>
#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QRegularExpression>
#include <QTextBrowser>
#include <QTextDocument>
#include <QUrl>
#include <QVBoxLayout>

namespace {

// "Ctrl+Shift+N" -> key caps. "Ctrl++" is Ctrl and "+".
QString keyCaps(const QString &combo)
{
    QStringList parts = combo.split(QLatin1Char('+'));
    QStringList keys;
    for (int i = 0; i < parts.size(); ++i) {
        if (parts[i].isEmpty()) { // "Ctrl++": the key is "+"
            keys << QStringLiteral("+");
            ++i;
            continue;
        }
        keys << parts[i];
    }
    QStringList caps;
    for (const QString &k : keys)
        caps << QStringLiteral("<span class=\"key\">&nbsp;%1&nbsp;</span>").arg(k.toHtmlEscaped());
    return caps.join(QStringLiteral("<span class=\"plus\"> + </span>"));
}

QString keyCaps(const QKeySequence &sequence)
{
    return keyCaps(sequence.toString(QKeySequence::NativeText));
}

// [[Ctrl+F]] in the guide's text -> key caps.
QString expandKeys(QString html)
{
    static const QRegularExpression re(QStringLiteral(R"(\[\[(.+?)\]\])"));
    QString out;
    qsizetype last = 0;
    auto it = re.globalMatch(html);
    while (it.hasNext()) {
        const auto m = it.next();
        out += html.mid(last, m.capturedStart() - last);
        out += keyCaps(m.captured(1));
        last = m.capturedEnd();
    }
    out += html.mid(last);
    return out;
}

// Rich text can't draw translucent colours (it drops the alpha and paints
// black), and some theme tokens are translucent: blend them first.
QString solid(const QColor &color, const QColor &under)
{
    const qreal a = color.alphaF();
    return QColor::fromRgbF(color.redF() * a + under.redF() * (1 - a), color.greenF() * a + under.greenF() * (1 - a),
                            color.blueF() * a + under.blueF() * (1 - a))
        .name();
}

} // namespace

HelpDialog::HelpDialog(const QList<ShortcutGroup> &shortcuts, QWidget *parent)
    : QDialog(parent), m_shortcuts(shortcuts)
{
    setObjectName("helpDialog");
    setWindowTitle(tr("Argonaut User Guide"));
    setAttribute(Qt::WA_DeleteOnClose);

    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto *side = new QWidget(this);
    side->setObjectName("helpSide");
    side->setFixedWidth(250);
    auto *sideLayout = new QVBoxLayout(side);
    sideLayout->setContentsMargins(12, 14, 12, 12);
    sideLayout->setSpacing(10);
    auto *title = new QLabel(tr("User Guide"), side);
    title->setObjectName("helpTitle");
    sideLayout->addWidget(title);
    m_search = new QLineEdit(side);
    m_search->setObjectName("helpSearch");
    m_search->setPlaceholderText(tr("Search the guide"));
    m_search->setClearButtonEnabled(true);
    sideLayout->addWidget(m_search);
    m_list = new QListWidget(side);
    m_list->setObjectName("helpTopics");
    m_list->setFrameShape(QFrame::NoFrame);
    m_list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_list->setTextElideMode(Qt::ElideRight);
    sideLayout->addWidget(m_list, 1);
    m_noMatch = new QLabel(tr("Nothing found."), side);
    m_noMatch->setObjectName("helpNoMatch");
    m_noMatch->hide();
    sideLayout->addWidget(m_noMatch);
    layout->addWidget(side);

    m_page = new QTextBrowser(this);
    m_page->setObjectName("helpPage");
    m_page->setFrameShape(QFrame::NoFrame);
    m_page->setOpenLinks(false);
    m_page->document()->setDocumentMargin(28);
    layout->addWidget(m_page, 1);

    buildTopics();
    for (const Topic &topic : std::as_const(m_topics)) {
        auto *item = new QListWidgetItem(topic.title, m_list);
        item->setData(Qt::UserRole, topic.id);
    }

    connect(m_list, &QListWidget::currentRowChanged, this, [this] { render(); });
    connect(m_search, &QLineEdit::textChanged, this, &HelpDialog::applySearch);
    connect(m_page, &QTextBrowser::anchorClicked, this, [this](const QUrl &url) {
        if (url.scheme() == QLatin1String("topic"))
            showTopic(url.path().isEmpty() ? url.host() : url.path());
        else
            QDesktopServices::openUrl(url);
    });

    m_list->setCurrentRow(0);
    resize(900, 640);
}

void HelpDialog::showTopic(const QString &id)
{
    if (!m_search->text().isEmpty())
        m_search->clear(); // shows every topic again
    for (int row = 0; row < m_list->count(); ++row) {
        if (m_list->item(row)->data(Qt::UserRole).toString() == id) {
            m_list->setCurrentRow(row);
            return;
        }
    }
}

void HelpDialog::changeEvent(QEvent *event)
{
    QDialog::changeEvent(event);
    if (event->type() == QEvent::ApplicationPaletteChange || event->type() == QEvent::PaletteChange)
        render(); // the page's colours come from the theme
}

void HelpDialog::render()
{
    QListWidgetItem *item = m_list->currentItem();
    if (!item || item->isHidden()) {
        m_page->clear();
        return;
    }
    const QString id = item->data(Qt::UserRole).toString();
    for (const Topic &topic : std::as_const(m_topics)) {
        if (topic.id != id)
            continue;
        const Theme t = currentTheme();
        m_page->document()->setDefaultStyleSheet(QStringLiteral(R"(
            body { color: %1; font-size: 10.5pt; }
            h1 { font-size: 20pt; font-weight: bold; margin-bottom: 4px; }
            h2 { font-size: 13pt; font-weight: bold; margin-top: 22px; margin-bottom: 6px; }
            p, li { line-height: 140%; }
            p.lead { color: %2; font-size: 11.5pt; margin-bottom: 14px; }
            a { color: %3; text-decoration: none; }
            .key { background-color: %4; color: %1; font-family: monospace; font-weight: bold; }
            .plus { color: %2; }
            .note { color: %2; }
            .tip { background-color: %5; }
            td.group { font-weight: bold; color: %3; padding-top: 16px; }
            td.keys { white-space: nowrap; }
            tr.odd { background-color: %5; }
        )")
                                                     .arg(solid(t.fg, t.window), solid(t.dimFg, t.window),
                                                          solid(t.accent, t.window), solid(t.pathBg, t.window),
                                                          solid(t.sidebar, t.window)));
        m_page->setHtml(pageHtml(topic));
        break;
    }
    applySearch();
}

QString HelpDialog::pageHtml(const Topic &topic) const
{
    QString body = topic.id == QLatin1String("shortcuts") ? shortcutsHtml() : expandKeys(topic.html);
    return QStringLiteral("<html><body><h1>%1</h1>%2</body></html>").arg(topic.title.toHtmlEscaped(), body);
}

QString HelpDialog::shortcutsHtml() const
{
    QString html = expandKeys(tr("<p class=\"lead\">Every shortcut, taken from the menus themselves. "
                                 "Most commands are also in the menu bar and the right-click menu.</p>"));
    html += QStringLiteral("<table width=\"100%\" cellspacing=\"0\" cellpadding=\"6\">");
    for (const ShortcutGroup &group : m_shortcuts) {
        html += QStringLiteral("<tr><td class=\"group\" colspan=\"2\">%1</td></tr>").arg(group.title.toHtmlEscaped());
        int n = 0;
        for (const Shortcut &s : group.shortcuts) {
            QStringList keys;
            for (const QKeySequence &k : s.keys) {
                if (!k.isEmpty())
                    keys << keyCaps(k);
            }
            QString shown = keys.join(QStringLiteral(" <span class=\"note\">%1</span> ").arg(tr("or")));
            if (!s.note.isEmpty())
                shown = expandKeys(s.note);
            if (shown.isEmpty())
                continue;
            html += QStringLiteral("<tr class=\"%1\"><td>%2</td><td class=\"keys\">%3</td></tr>")
                        .arg(n++ % 2 ? QStringLiteral("odd") : QStringLiteral("even"), s.action.toHtmlEscaped(), shown);
        }
    }
    html += QStringLiteral("</table>");
    return html;
}

void HelpDialog::applySearch()
{
    const QString query = m_search->text().trimmed();
    // Show only the topics that mention every word (accents and case don't matter).
    const QStringList words = DirectoryModel::foldForSearch(query).split(QLatin1Char(' '), Qt::SkipEmptyParts);
    int shown = 0;
    int firstShown = -1;
    for (int row = 0; row < m_list->count(); ++row) {
        QListWidgetItem *item = m_list->item(row);
        bool match = true;
        if (!words.isEmpty()) {
            const Topic &topic = m_topics[row];
            QTextDocument doc;
            doc.setHtml(topic.id == QLatin1String("shortcuts") ? shortcutsHtml() : topic.html);
            const QString text = DirectoryModel::foldForSearch(topic.title + QLatin1Char(' ') + doc.toPlainText());
            for (const QString &w : words)
                match = match && text.contains(w);
        }
        item->setHidden(!match);
        if (match) {
            ++shown;
            if (firstShown < 0)
                firstShown = row;
        }
    }
    m_noMatch->setVisible(shown == 0);
    QListWidgetItem *current = m_list->currentItem();
    if ((!current || current->isHidden()) && firstShown >= 0) {
        m_list->setCurrentRow(firstShown); // renders, then comes back here
        return;
    }
    if (shown == 0) {
        m_page->clear();
        return;
    }

    // Highlight the words on the page.
    QList<QTextEdit::ExtraSelection> marks;
    if (!words.isEmpty()) {
        const Theme t = currentTheme();
        QTextCharFormat format;
        QColor mark = t.accent;
        mark.setAlpha(80);
        format.setBackground(mark);
        const QStringList raw = query.split(QLatin1Char(' '), Qt::SkipEmptyParts);
        for (const QString &w : raw) {
            QTextCursor cursor(m_page->document());
            while (true) {
                cursor = m_page->document()->find(w, cursor);
                if (cursor.isNull())
                    break;
                marks.append({cursor, format});
            }
        }
        if (!marks.isEmpty()) { // scroll to the first match
            QTextCursor first = marks.first().cursor;
            first.clearSelection();
            m_page->setTextCursor(first);
            m_page->ensureCursorVisible();
        }
    }
    m_page->setExtraSelections(marks);
}

void HelpDialog::buildTopics()
{
    auto add = [this](const char *id, const QString &title, const QString &html) {
        m_topics.append({QString::fromLatin1(id), title, html});
    };

    add("start", tr("Getting Started"), tr(R"(
<p class="lead">Argonaut is a fast file manager. It works like GNOME Files (Nautilus), shares its
bookmarks, recent files, thumbnails and Trash, and adds a few things of its own.</p>
<h2>The window</h2>
<ul>
<li><b>Sidebar</b> (left, [[F9]] shows or hides it): Recent, Starred, your folders, bookmarks, drives and the Trash.</li>
<li><b>Path bar</b> (top): click a part of the path to go there. Click empty space in it, or press [[Ctrl+L]], to type a path.</li>
<li><b>Files</b>: as a grid with thumbnails ([[Ctrl+1]]) or a list with columns ([[Ctrl+2]]).</li>
<li><b>Floating bar</b> (bottom right): what's selected, its size, and short messages.</li>
</ul>
<h2>Moving around</h2>
<ul>
<li>Double-click a folder, or select it and press [[Enter]].</li>
<li>Back and Forward: [[Alt+Left]] / [[Alt+Right]], or the mouse's side buttons. Up one folder: [[Alt+Up]]; Down: [[Alt+Down]] goes into the selected folder, or back into the one Up came from (Up selects it). The four arrows at the top left of the header do the same. Home: [[Alt+Home]].</li>
<li>Type the start of a name to jump to it.</li>
</ul>
<h2>Tabs and windows</h2>
<ul>
<li>[[Ctrl+T]] opens a tab, [[Ctrl+W]] closes it, [[Ctrl+Shift+T]] brings back the last one you closed.</li>
<li>Middle-click a folder or a sidebar entry to open it in a new tab in the background.</li>
<li>[[Ctrl+Tab]] goes to the next tab and [[Alt+1]]… [[Alt+9]] to a given one.</li>
</ul>
<h2>Any command by name</h2>
<p>Press [[Ctrl+Shift+P]] and type a few letters of what you want: “hid” for Show Hidden Files,
“nf” for New Folder, “sort size”. [[Enter]] runs it, [[Esc]] closes. The commands you used last come first.
Commands that come from an extension start with <b>ext::</b> (“ext::System Cleanup”), so “ext” lists them all.</p>
<p>See also: <a href="topic:views">Views and sorting</a>, <a href="topic:shortcuts">Keyboard shortcuts</a>.</p>
)"));

    add("views", tr("Views, Sorting and Previews"), tr(R"(
<h2>Grid and list</h2>
<p>[[Ctrl+1]] shows a grid, [[Ctrl+2]] a list. Zoom with [[Ctrl++]] and [[Ctrl+-]], or hold [[Ctrl]]
and turn the mouse wheel. [[Ctrl+0]] goes back to the normal size. Each view remembers its own size.</p>
<h2>Sorting</h2>
<p>Click a column header in the list, or use <b>View → Sort</b>. Folders always come first, and names
are in natural order, so “file2” comes before “file10”.</p>
<h2>Opening folders in place</h2>
<p>Turn on <b>Expandable folders</b> in Preferences ([[Ctrl+,]]) to see an arrow before each folder in the list.
Click it, or press [[Right]], to show the folder's items right below it; [[Left]] closes it again, or goes
to the folder an item is in. You can copy, move, rename and preview items there as anywhere else.
Search, the Trash, Recent and Starred always show a plain list.</p>
<h2>Columns</h2>
<p>The list shows Name, Size, Type and Modified. Right-click the column header to add <b>Owner</b>,
<b>Group</b> and <b>Permissions</b>.</p>
<h2>The list in columns</h2>
<p>In Preferences ([[Ctrl+,]]) → List View, <b>Columns</b> lays the list out in two, three or four columns
side by side: each item is an icon, its name and its size, read across and then down. Everything else works
as in the list: selecting, renaming, dragging, the arrow keys. Folders can't be opened in place there, and
the order is chosen with <b>View → Sort</b>. Choose <b>One</b> to have the table with its columns again.</p>
<h2>Fade between folders</h2>
<p>When you open another folder, the old one fades into the new one for a moment. Nothing loads later because
of it, and it steps aside when you go through folders quickly. <b>Preferences → Motion</b> switches it on or
off. Until you choose there, it follows the desktop: off where animations are switched off.</p>
<h2>Hidden files</h2>
<p>[[Ctrl+H]] shows or hides dot-files and backups (names ending in ~).</p>
<h2>Quick Look</h2>
<p>Select a file and press [[Space]] for a preview without opening an app: photos, PDFs, videos, fonts,
text and code, what's inside archives, and folders. Markdown is shown formatted, an HTML file as a page to
read (its text, headings, lists, tables and links, without its styles, scripts and pictures: nothing in it is run
and nothing is fetched from the internet), and CSV files and
spreadsheets as a table (the <a href="topic:extensions">Document Preview extension</a>; with it switched off an
HTML file is shown as its source text). The preview is a pane at the right of the files;
drag its edge to make it wider. It follows the file you select, so the arrow keys go through a folder.
What an extension shows there (the System Dashboard, the running services, a diff) <b>stays while you go on working
with the files</b>: selecting files and changing folder don't take it away. Press [[Space]] on a file to see that
file's preview again; [[Esc]] or the pane's ✕ closes it.
<b>Each of them gets a tab of its own</b> at the top of the pane, after <i>Preview</i> (the selected file's):
open the dashboard and then the journal, and both stay, a click apart, until you close their tab (its ×)
or the pane. Only the chosen tab is kept up to date; another one is made again when you come back to it,
with what you had typed in its search field.
[[Space]] or [[Esc]] closes it, and it remembers its width.</p>
<p><b>Text in the preview can be copied and searched.</b> Click in it: then select with the mouse (or
[[Ctrl+A]] for all) and [[Ctrl+C]] copies the words. [[Ctrl+F]] opens a small find bar under the text: what you
type is marked everywhere it is, any case; [[Enter]] goes to the next, [[Shift+Enter]] to the one before, [[Esc]]
closes the bar. While the keys are in the text, the file commands (Delete, Copy…) leave the files alone;
[[Esc]] gives the keys back to the files.</p>
<p><b>A text file can be edited in the preview.</b> Tick <b>Edit</b> at the top of the pane, or press
[[Ctrl+Space]] on the file (also <b>File → Edit in Preview</b>; it is offered for text files only). The text is then on white paper, like a notepad, whatever the theme.
It is a code editor: line numbers, the colours of the file's language (C and C++, Python, JavaScript and TypeScript,
JSON, HTML, XML, CSS, Markdown, YAML, shell scripts, SQL, CMake, Makefiles, settings files and more, chosen by the
file's name), matching brackets, and the arrows in the margin fold a block away. [[Ctrl+F]] finds in it.
Type as in any editor; [[Ctrl+S]] or the
<b>Save</b> button saves. <b>The first save puts the file as it was in the Trash</b>, so you can always get it back;
the line ends and encoding stay as the file had them. While you edit, the file keeps the pane (selecting other
files doesn't take it), and closing the pane or the window with unsaved changes asks first. If another program
changes the file meanwhile, you are asked before your version replaces it (theirs goes to the Trash). Only text
files you may change can be edited: not programs, pictures or documents, not in the Trash, and not over 5 MB.</p>
<p>A PDF is shown whole: every page, one under another. Scroll with the wheel or the scrollbar; the line under
the pages says which page you are on. [[Ctrl]] and the wheel make the pages bigger. Pages are drawn as you reach
them, so a long document opens as quickly as a short one. They are pictures: to select text, follow a link or fill
in a form, use <b>Open</b>. The reader needs <i>poppler-utils</i> and <i>bubblewrap</i>; a PDF that is locked with
a password is not shown.</p>
<h2>Split view</h2>
<p>[[F3]] shows two folders side by side. [[F5]] copies the selection to the other side and [[F6]]
moves it; you can also drag between them. Every shortcut acts on the side you're working in.</p>
)"));

    add("select", tr("Selecting and Renaming"), tr(R"(
<h2>Selecting</h2>
<ul>
<li>Click to select, [[Ctrl]]+click to add or remove one item, [[Shift]]+click for a range, or drag a box around items.</li>
<li>[[Ctrl+A]] selects everything, [[Ctrl+Shift+I]] inverts the selection.</li>
<li><b>Select Items Matching</b> ([[Ctrl+S]]): type a pattern such as <code>*.jpg</code> or <code>IMG_20??*</code>.
Upper and lower case don't matter, and text without <code>*</code> or <code>?</code> selects every name that contains it.</li>
</ul>
<h2>Renaming one item</h2>
<p>Press [[F2]] and type. The name is selected without its extension, so you can just type the new name.
[[Enter]] keeps it and [[Esc]] cancels.</p>
<h2>Renaming many items</h2>
<p>Select several items and press [[F2]]:</p>
<ul>
<li><b>Rename using a template</b>: build the name from <code>[Original name]</code> and a number:
<code>[1]</code>, <code>[01]</code> or <code>[001]</code>. Choose the first number and the order (by name or by date).</li>
<li><b>Find and replace text</b>: change part of every name, with or without matching case.</li>
</ul>
<p>Extensions are always kept. The preview shows every new name before anything changes, and a name
that can't be used is shown in red with the reason. One [[Ctrl+Z]] undoes the whole batch.</p>
<h2>New items</h2>
<p>[[Ctrl+Shift+N]] makes a folder and [[Ctrl+Alt+N]] an empty file. <b>File → New Document</b> lists the
files in your Templates folder; put any document there (a letter, a spreadsheet) to reuse it as a starting point.</p>
)"));

    add("files", tr("Copying, Moving and Undo"), tr(R"(
<h2>Copy, cut and paste</h2>
<p>[[Ctrl+C]], [[Ctrl+X]] and [[Ctrl+V]] work as usual, also with files copied in other file managers.</p>
<h2>Drag and drop</h2>
<p>Dropping on the same drive moves, onto another drive copies. Hold [[Ctrl]] to copy or [[Shift]] to move.
Drop onto a folder, a tab, a sidebar entry or the Trash. Hold a dragged item still over a folder (or a sidebar
entry) for a moment and it opens, so you can go deeper before dropping.</p>
<h2>Nothing is ever overwritten</h2>
<p>When a name is taken, Argonaut asks: <b>Replace</b> (the old item goes to the Trash, it isn't deleted),
<b>Skip</b>, <b>Keep Both</b> or, for folders, <b>Merge</b>. Copies inside the same folder become “name (copy)”.</p>
<h2>Progress</h2>
<p>Long operations show a card at the bottom left with the time left and a Cancel button. A cancelled
copy leaves nothing half-finished behind.</p>
<h2>USB sticks</h2>
<p>On removable drives Argonaut says “done” only after the data has really been written to the drive,
so it's safe to unplug it then. Use the ⏏ button in the sidebar to eject it.</p>
<h2>Undo and redo</h2>
<p>[[Ctrl+Z]] undoes copies, moves, trashing, renames, new items, extracting and compressing.
[[Ctrl+Shift+Z]] or [[Ctrl+Y]] redoes. Undoing a copy moves the copies to the Trash; nothing is deleted for good.</p>
<h2>Items you can't change</h2>
<p>Files and folders you aren't allowed to change show a 🔒 badge. If their folder doesn't let you change it,
Cut, Move to Trash, Rename and Delete are greyed out; you can still copy them.</p>
)"));

    add("search", tr("Searching"), tr(R"(
<p>Press [[Ctrl+F]] and type. Argonaut searches the open folder <b>and all its sub-folders</b>, and
results appear as you type.</p>
<ul>
<li>Several words narrow the search down: “invoice 2024” finds names that contain both.</li>
<li>Case and accents don't matter: “cafe” finds “Café”, “θαλασσα” finds “Θάλασσα”.</li>
<li>The Location column (or the tooltip in the grid) shows where each result is. Right-click →
<b>Open Item Location</b> goes there.</li>
<li>Hidden folders are searched only while hidden files are shown. Other drives and links aren't followed.</li>
<li>[[Esc]] or clearing the field shows the folder again.</li>
</ul>
<h2>Filters</h2>
<p><b>Filters</b> next to the search field narrows results by type (folders, documents, images, audio, video,
archives, text and code) and by when they changed. Filters also work without typing anything: “all images changed
in the last 7 days, anywhere below here”. They're cleared when the search closes.</p>
<h2>Inside files</h2>
<p>Click <b>Inside files</b> next to the search field (or View → Search Inside Files) to also find files by
the words in them. The Location column then shows the text around the match.</p>
<ul>
<li>Text files (notes, code, csv, json…) are read wherever you search, on this computer's own disks.</li>
<li>Documents such as PDFs and office files are found through GNOME's search index, in the folders it
indexes (Settings → Search → Search Locations). There, upper and lower case don't matter, but accents do.</li>
</ul>
)"));

    QString places = tr(R"(
<p class="lead">Recent, Starred, bookmarks, drives and the Trash: the sidebar's places.</p>
<h2>Recent</h2>
<p>What you opened lately, in Argonaut or in any other app, newest first. Right-click → <b>Remove from Recent</b>.
If you turned off “remember recent files” in GNOME's privacy settings, Argonaut doesn't add to it.</p>
<h2>Starred</h2>
<p>Right-click → <b>Star</b> marks a file or folder with ★ and lists it under Starred. Stars follow items
when you rename or move them.</p>
<h2>Bookmarks</h2>
<p>[[Ctrl+D]] bookmarks the open folder (press it again to remove it). You can also drop a folder on empty
space in the sidebar. Right-click a bookmark to rename or remove it. Bookmarks are shared with GNOME Files.</p>
<h2>Servers</h2>
<p><b>Connect to Server…</b> (in the sidebar and the Go menu) opens folders on other computers:
<code>sftp://me@example.com</code> (SSH), <code>smb://nas/photos</code> (Windows and NAS shares),
<code>ftp://…</code> and <code>davs://…</code> (WebDAV). You can also type an address in the path bar ([[Ctrl+L]]).
Argonaut asks for a login when the server needs one; <code>smb://server</code> on its own lists its shared folders.</p>
<p>This is the <a href="topic:extensions">Connect to Server extension</a>, which comes with Argonaut. If you
never connect to servers, switch it off in <b>Edit → Extensions</b>: its entries leave the sidebar and the Go menu.</p>
<p>Connected servers appear at the end of the sidebar; the ⏏ button disconnects. Copying, moving, renaming and
previews work there like anywhere else. Servers have no Trash, so deleting there is permanent (Argonaut asks first).</p>
<h2>Drives</h2>
<p>Plugged-in drives appear in the sidebar. The ⏏ button ejects one safely. While a copy to or from the drive is still running, Argonaut asks you to wait.</p>
<p>A drive that is there but not mounted (a second disk, a stick the desktop didn't open) is in the sidebar too,
as it is in Files. Click it: the desktop mounts it, asking for a password itself if it needs one, and Argonaut
opens it. Its ⏏ then unmounts it again. The terminal version lists these drives in its places as well.</p>
)");
#if defined(Q_OS_LINUX)
    places += tr(R"(
<h2>Trash</h2>
<p>[[Delete]] moves items to the Trash, and [[Shift+Delete]] deletes them for good after asking. In the Trash,
<b>Restore</b> puts items back where they came from, <b>Restore All</b> does that for everything, and
<b>Empty Trash</b> deletes everything. The Trash shows
items from every drive, trashed by any app.</p>
)");
#endif
    add("places", tr("Places and Drives"), places);

    if (FileJob::archivesSupported()) {
        add("archives", tr("Archives"), tr(R"(
<h2>Extracting</h2>
<p>Right-click an archive → <b>Extract Here</b> or <b>Extract to…</b>. Argonaut opens zip, 7z, tar.gz/xz/bz2/zst,
rar, iso, deb and more. One item inside comes out as it is; several go into a folder named after the archive.</p>
<p>It's safe: nothing is overwritten, and entries that try to write outside the folder are skipped and reported.
Password-protected archives aren't supported yet.</p>
<h2>Compressing</h2>
<p>Select items and choose <b>Compress…</b>. Pick .zip (works everywhere), .tar.xz (smaller), .tar.gz (fast)
or .7z (smallest). Both directions show progress, can be cancelled and can be undone.</p>
<h2>Looking inside</h2>
<p>[[Space]] on an archive lists what's in it without extracting anything.</p>
)"));
    }

    add("extras", tr("Email, Background and Scripts"), tr(R"(
<ul>
<li><b>Send by Email…</b> (right-click files) opens your email app with the files attached; you write and send
the message there. Folders can't be attached: compress them first.</li>
<li><b>Set as Background</b> (right-click a picture) copies it into your backgrounds folder and puts it on the desktop.</li>
<li><b>Scripts ▸</b> lists the programs in <code>~/.local/share/nautilus/scripts</code>, the same folder GNOME Files
uses, and runs them on the selected items with the same variables (<code>NAUTILUS_SCRIPT_SELECTED_FILE_PATHS</code>…),
so existing scripts work unchanged. Make a script executable to see it there.</li>
</ul>
)"));

    add("types", tr("Real File Types and Safety"), tr(R"(
<p class="lead">Most file managers trust a file's name. Argonaut also looks at what's inside.</p>
<ul>
<li>An <b>amber ! badge</b> means the content doesn't match the name, for example a “.pdf” that is really a web page
from a failed download. Hover over it for details. Right-click → <b>Rename to “…”</b> fixes the name.</li>
<li>A <b>red ! badge</b> means a <b>program disguised as a document</b>. Argonaut warns you and never runs it.</li>
<li>Files open with the app for what they really are, so a mislabelled file still opens correctly.</li>
<li><b>Properties</b> ([[Ctrl+I]]) shows both what the name says and what the file is, and for photos, music,
videos and PDFs their details: size in pixels, camera and exposure, date taken, length, artist, pages…</li>
</ul>
<h2>Badges</h2>
<ul>
<li>★ top right: starred.</li>
<li>🔒 top left: you can't change this item.</li>
<li>↗ bottom left: a link to another file or folder. The tooltip says where it points.</li>
</ul>
<p>Checking types doesn't slow anything down: folders appear at once from the names, the check runs in the
background, and each file is read only once until it changes.</p>
)"));

    add("analyze", tr("Disk Usage"), tr(R"(
<p>[[Ctrl+3]] (or the pie button) shows how much space every folder uses, as a tree with bars, biggest first.
Folders appear while they're measured.</p>
<ul>
<li>Sizes are space on disk, exactly like <code>du</code>; hard links count once, and other drives aren't included.</li>
<li><b>View → Show Files When Analyzing</b> also lists the biggest files in each folder.</li>
<li>Double-click a folder to look inside it; that's instant within what's been measured.</li>
<li>Trashing, moving or copying updates the sizes at once. <b>Scan Again</b> picks up changes made by other programs.</li>
<li>[[Ctrl+1]] or [[Ctrl+2]] goes back to the files.</li>
</ul>
)"));

    add("organize", tr("Organizing with Claude"), tr(R"(
<p class="lead">Claude, Anthropic's AI, can propose a tidy structure for a messy folder and tell you what's in it.</p>
<h2>Before the first time</h2>
<p>Add your Anthropic API key in <b>Preferences → Claude</b> (get one in the Claude Console) and press <b>Test</b>;
that check is free. The key is kept in your keyring.</p>
<h2>Organizing a folder</h2>
<ol>
<li>Open the folder and choose <b>File → Organize with Claude…</b> (or right-click empty space).</li>
<li>Optionally write your wishes (“invoices by year”) and choose what Claude may see. You see the estimated cost
before anything is sent; nothing is sent until you press <b>Start</b>.</li>
<li>When names say too little, Claude may ask to read the beginning of a few files: you choose
<b>Allow</b> or <b>Don't share</b>. Programs are never read.</li>
<li>While Claude is asked, the preview pane at the right says <b>Waiting for Claude's answer</b>, with how long
it has been; it can take a minute. <b>Stop Waiting</b> gives up on it (what was already asked is still paid for).</li>
<li>The plan appears in the preview pane at the right: the report, then every change with a tick. Click a change
to see its file in the folder; you can look around meanwhile, the plan stays. Untick what you don't want and press
<b>Apply</b>, or <b>Dismiss</b>.</li>
</ol>
<p>Claude never touches your files: it only proposes. Argonaut checks every change (one it won't do is shown in red
with the reason), checks again when you press Apply in case the folder changed meanwhile, creates the folders and
moves and renames the items you kept, never overwriting anything. [[Ctrl+Z]] puts everything back (once for the
moves, once more for the new folders).</p>
<p>This is the <b>Organize with Claude</b> extension (<a href="topic:extensions">Edit → Extensions</a>): switch it
off there if you don't want it. It never has your key: it tells Argonaut what to ask, and Argonaut asks.</p>
<h2>Privacy and cost</h2>
<p>What is sent: the names, sizes, dates and types of the folder's items (up to 500), and only if allowed, the beginning
of some files (some text, the first pages of a PDF, a small preview of an image). <b>Preferences → Claude</b> sets the
default (names only, ask, or read when needed) and the model, and shows what you've spent.</p>
<p><b>Every answer is kept.</b> Whatever asks Claude for you (Organize, the reports on services, processes and the
journal, any extension), each answer is written as a page in the folder <code>claudeReports</code> of Argonaut's data
(<code>~/.local/share/argonaut/claudeReports</code>): <b>Go → Claude Reports</b> opens it. A page is named after what
asked and when, for example <code>running_services_report_17.11.26_12.54.31.md</code> (day, month, year, then the
time), and holds Claude's answer and under it everything that was sent: the instructions the program gave Claude, the
whole list or conversation, what you typed with it, and the settings. Pictures and files that went along are named
with their size, not copied; your API key is never in it. The pages are Markdown, so Space shows them formatted.
Only you can read them. Argonaut never deletes them: remove the ones you don't want.</p>
<p><b>Analyze with Claude</b> under the list of <a href="topic:extensions">running services</a> sends each service's
name, description, memory, whose it is, and the path of its program and of its unit file, with what you typed beside
the button. A program's arguments are never sent. It is one request; what it cost is said under the report.
Under the list of running processes it sends each process's name, ID, the ID of the process that started it, whose
it is, its memory and the program that was started (the 400 biggest), and never its arguments. Under the System
Journal it sends the messages themselves (the 150 most important kinds, with who wrote them, how often and when),
which can name users, computers, addresses and files. <b>Tell Claude What to Do Here…</b> sends your sentence and the names, sizes, dates and types of the open folder's
items, never their contents. Under <b>Updates and Restart</b>, <b>Explain with Claude</b> sends the waiting updates' names, versions and sources, the
system's name and kernel, and why a restart is needed; not which files your programs use.
<b>Ask Claude: How to Install…</b> sends what you typed, the system's name and version,
its processor type, the desktop, which package tools it has and, for packages Claude asks about, whether they are
installed and which version the system offers; nothing about your files. <b>Ask Claude: What Is This About?</b> sends what it reads of
the one file or folder you chose: the beginning of a text, a document's text, a small copy of a picture, or the names
inside a folder or an archive.</p>
)"));

    add("git", tr("Git Repositories"), tr(R"(
<p>Inside a Git repository, Argonaut shows what changed, like a code editor does.</p>
<ul>
<li>A letter after the name in the list (a small tag on the icon in the grid):
<b>M</b> changed, <b>A</b> added, <b>U</b> new and not added yet, <b>C</b> a merge conflict.</li>
<li>A dot on a folder means something inside it changed.</li>
<li>The path bar shows the branch and how many items changed. Hover over it for the repository's folder.</li>
<li>It updates when files change, after you run <code>git add</code> or <code>git commit</code>, and when you come back to the window.</li>
</ul>
<h2>The Git menu</h2>
<p>Click the branch in the path bar, or right-click the folder's empty space and choose <b>Git</b>:</p>
<ul>
<li><b>Show Changes</b> lists the changed files of the whole repository in the file pane, like a folder.
Select one and press [[Space]]: the preview pane shows what changed in it, added lines on green, removed
lines on red. The arrow keys go from file to file. The list follows what you stage, commit or edit;
[[Alt+Left]] goes back to the folder. Deleted files are not in the list (there is nothing to show as a file).</li>
<li><b>Show Changes as One Diff</b> puts everything that changed in the preview pane as one text. Right-click
changed files → <b>Git → Show Changes</b> for those only.</li>
<li><b>Commit…</b> asks for a message. It commits what is staged; when nothing is staged, every changed file
Git already tracks. The entry says which, and how many. New files have to be staged first.</li>
<li><b>Switch Branch</b> lists the branches, the current one ticked. <b>New Branch…</b> asks for a name and switches to it.</li>
<li>On changed files: <b>Stage</b>, <b>Unstage</b>, and <b>Discard Changes…</b>, which puts a file back to
its last committed version after asking. That can't be undone, and nothing goes to the Trash.</li>
</ul>
<p>Argonaut runs Git with the repository's <b>hooks switched off</b>, so a pre-commit hook does not run for a
commit made here. Pull and Push are not offered: use the terminal ([[Ctrl+`]]).</p>
<p>For safety, a repository whose own settings would make Git run programs (for example one you downloaded)
is not checked; the path bar then says “git: off”. Very big repositories that take too long are skipped too.
This is the Git extension: switch it off in <b>Edit → Extensions</b> ([[Ctrl+Shift+X]]) if you don't want it
(see <a href="topic:extensions">Extensions</a>).</p>
)"));

    add("extensions", tr("Extensions"), tr(R"(
<p>Extensions are small programs that add to Argonaut: a mark on some files (a letter or a dot, like Git's
<b>M</b>), a short text at the right end of the path bar, and commands in the right-click menu. These come
with Argonaut: <b>Git</b> (marks, the branch, and <i>Git: Stage</i> / <i>Git: Unstage</i> on changed items),
<b>Organize with Claude</b>, <b>Send by Email</b>, <b>Set as Background</b>, <b>Document Preview</b>, <b>System Dashboard</b>, <b>Space Overview</b>, <b>Running Services</b>, <b>Running Processes</b>, <b>System Journal</b>, <b>Network Connections</b>, <b>System Cleanup</b>, <b>Updates and Restart</b>, <b>How to Install</b>,
<b>What Is This About</b>, <b>Tell Claude What to Do</b>, <b>How to Install</b>, <b>Project Explorer</b> and <b>Connect to Server</b> (see <a href="topic:places">Places and Drives</a>).</p>
<p><b>Project Explorer</b>: <b>File → Open as Project</b> (or a folder's right-click menu) shows the folder as a tree under
the places, with a divider you can drag between them. What the project's Git ignores (<i>build/</i>, <i>node_modules/</i>)
is faint and <i>.git</i> is left out. A click on a folder opens it in the files, a click on a file shows it selected in its
folder (and in the preview, if that is open), a double click opens it. <b>File → Open Recent Project…</b> chooses one of the
last ten, <b>Close Project</b> or the tree's ✕ closes it. The project is there again the next time Argonaut starts.</p>
<p>An extension that says so may ask Claude things <b>through Argonaut</b>: with the key and the model you set in
<b>Preferences → Claude</b>, at your cost, and only while it does something you chose. It never gets the key. The
Extensions window says which extensions use Claude and what each has cost so far.</p>
<p>An extension can also <b>propose changes</b> to a folder: moves and new names. They appear in the preview pane
with a tick each; Argonaut checks them, does what you keep through its own file engine, and [[Ctrl+Z]] undoes
it. The extension itself moves nothing.</p>
<p>An extension may ask you something while it works, such as a login. The window that asks is Argonaut's and
always says which extension is asking; what you type there, a password included, is handed to that extension.</p>
<p><b>System Dashboard</b>: choose <b>View → System Dashboard</b>, or find it in the command palette
([[Ctrl+Shift+P]]). The preview
pane shows how busy the computer is: the processor, the memory and the disk the folder is on as gauges, what
the system is, and the network's download and upload. It updates every two seconds until you select a file
or close the pane, and scrolls where it is longer than the pane.</p>
<p>Under the gauges is its <b>Health</b> card: the <i>load</i> (the average over the last 1, 5 and 15 minutes,
against the number of processor cores), how much <i>swap</i> is in use, the <i>temperature</i> where the computer
has sensors, and a line for each <i>drive</i> with what it says of itself (SMART): OK, how many bad sectors a hard
disk has found, how worn an NVMe drive is, its temperature, or that it is failing. A value worth a look is orange,
a bad one red: a load above the number of cores, swap more than half used, 85 °C and more, bad sectors, a failing
drive. The drives' data comes from the desktop's disk service, without asking for a password; a computer without it
(a server) shows no drive lines. Then <b>Disks</b>: every disk that is mounted, with how full it is (orange from
75%, red from 90%).</p>
<p><b>Space Overview</b>: choose <b>View → Space Overview</b>, or find it in the command palette. The preview
pane shows how big the open folder is, how full its disk is, and the biggest folders and files directly inside
it, each with a bar for its share. It is a quick look and only shows: it measures when you choose it (choose it
again to measure again), and to open folders in the tree or delete from it you use
<a href="topic:analyze">Analyze</a>.</p>
<p><b>Running Services</b>: choose <b>View → Running Services</b>, or find it in the command palette. The preview
pane shows the services that are running now, the one using the most memory first: the system's and those of your
own session (marked <i>user</i>), with what each is and a bar against the biggest. It updates every three
seconds until you select a file or close the pane. It only shows: nothing is started or stopped. A service's memory
is that of all its programs together, as systemd counts it. Beside each name is an <b>ⓘ</b>: rest the mouse on it,
or click it, and a tooltip says what the service is (its description, in systemd's own words), its full unit name
and whether it is the system's or your session's. That is where to look when the pane is too narrow to show the
description after the name. <b>A service's memory is that of all the programs in its group</b>, and for some
services most of it belongs to programs the service only started: the session's <i>dbus</i> is the usual one (it
starts the calendar, the online accounts and other helpers when they are asked for). Where a service's memory is
more than one program's, its ⓘ lists them, biggest first, so a large number can be traced to the program behind
it.</p>
<p><b>Services that failed</b> (they didn't start, or stopped when they shouldn't have) are the first rows of the
list, in red, with <i>failed</i> where the memory would be; the heading says how many there are, or <i>none
failed</i>. The ⓘ of such a row also says how it failed, in systemd's words, and the command that shows why
(<code>systemctl status …</code>). Search for "failed" to see only those. Claude is told about them too, and its
report starts with them.</p>
<p>Under the list is <b>Analyze with Claude</b>, with a field at its left. Press the button and Claude reads the
list as it is now and writes a report: the overall picture, anything worth a closer look and why (a program running
from an unusual folder, a name that imitates a well-known service, far more memory than such a service needs) with a
command that only inspects it, and what each service is for. Type something in the field first ("what is the bus
service doing", "tell me only about the bus service") and Claude answers that. <b>The list stays where it is</b>: the pane splits, and under the list it says it is waiting (with a
<b>Stop Waiting</b> button) and then shows the report. Drag the divider between the two to give either more room;
the ✕ on the report closes it. Ask again, with another question, and the new answer replaces it. It needs your Claude API key (Preferences → Claude) and is billed to your Anthropic account; the cost of
each report is said under it. It is Claude's reading of names and paths, not a security scan, and nothing is ever
started, stopped or changed.</p>
<p><b>Tell Claude What to Do</b>: choose <b>File → Tell Claude What to Do Here…</b> (or right-click the folder's
empty space) and say in your own words what should be done in the open folder: "move every PDF older than a year into
Archive", "make a zip of the photos from August", "put the invoices in folders by year". Claude answers with the
steps it proposes, and they appear in the preview pane as a checklist, as Organize's do: tick what you keep, press
<b>Apply</b>, and [[Ctrl+Z]] undoes it (one kind of step at a time).</p>
<p><b>Claude changes nothing itself</b>, and what it may propose is narrow, whatever is asked and whatever a file
is called:</p>
<ul>
<li>A step is one of these and nothing else: <b>move</b>, <b>rename</b>, <b>copy</b>, <b>compress</b> into an
archive, a <b>new folder</b>, or <b>move to the Trash</b>. Nothing is ever deleted for good, and nothing is run.</li>
<li>A step is about an item of the open folder. Anything else (another folder, a system file) is refused by
Argonaut, shown in red, and can't be ticked.</li>
<li>It works only in your own folders and drives, not in the system's.</li>
<li>Nothing happens before you press Apply, and no step replaces something that is already there.</li>
</ul>
<p>Claude is sent your sentence and the names, sizes, dates and types of the folder's items (500 at most), never what
is in the files. It needs your Claude API key, and the plan's note says what it cost.</p>
<p><b>What Is This About</b>: right-click a file or a folder and choose <b>Ask Claude: What Is This About?</b>
Claude reads it and answers in a few lines in the preview pane: one sentence saying what it is, then the points
that matter (for a bill who it is from, the date and the amount; for code what it does; for a picture what it
shows). The answer stays in the pane while you go on; [[Space]] on a file brings that file's preview back. What is
read depends on the kind of file, and the line under the answer says what was sent:</p>
<ul>
<li><b>Text, code, data:</b> the beginning (the first 24 kB). <b>Web pages:</b> their words, without styles and
scripts.</li>
<li><b>PDFs:</b> the text of the first pages, read in a sandbox. A small PDF without text (a scan) is sent as it
is, for Claude to read.</li>
<li><b>Office documents</b> (docx, odt, pptx, odp, xlsx, ods): their text, read in a sandbox.</li>
<li><b>Pictures:</b> a small copy (1024 pixels at most).</li>
<li><b>Archives and folders:</b> the names of what is inside, with sizes. Nothing is unpacked.</li>
<li><b>Music and video:</b> their details (length, title, format), not the sound or the pictures.</li>
</ul>
<p>A program is never read or sent, nor a file that only looks like a document. What can't be read gets a line
saying why, and Claude isn't asked. It needs your Claude API key (Preferences → Claude), costs a little each time
(said under the answer), and every answer is kept with the other reports (Go → Claude Reports).</p>
<p><b>System Cleanup</b>: choose <b>View → System Cleanup</b>, or find it in the command palette. The preview pane
measures what can safely be removed to free disk space, biggest first, each with its size and what removing it means:
packages no longer needed, old kernels (the one running and the newest other one always stay), the downloaded package
files, old Snap revisions, Docker's unused images, stopped containers and build cache (never its volumes, which hold
data), the journal beyond its newest 200 MB, crash reports, your programs' caches and the Trash. The <b>ⓘ</b> beside a row
says more. <b>It removes nothing by itself.</b> Under the list are the commands that free the system's part, to read and
run yourself in a terminal (those with sudo ask for your password). Your own caches are freed with <b>Move My Caches to
the Trash…</b> under the list: they appear as a checklist, what stays ticked goes to the Trash, and [[Ctrl+Z]] brings it
back. Programs make their caches again, more slowly the first time, so close them first. The space of what is in the
Trash returns when you empty it. Nothing leaves this computer.</p>
<p><b>Updates and Restart</b>: choose <b>View → Updates and Restart</b>, or find it in the command palette. The
preview pane lists the updates waiting for this computer, security fixes first: the system's packages, Snaps, Flatpaks
and firmware, each with the new version and the one installed. At the top it says whether the computer needs a
restart and why (the system asks for one, or a newer kernel is installed), and which of your programs still use a
library that was updated: those run the old version until you close and open them again. It also says how old the
package lists are (run <code>sudo apt update</code> when they are old) and whether security updates install by
themselves. <b>It installs nothing</b>: under the list are the commands to run yourself in a terminal. The field above
the list searches; <b>Explain with Claude</b> under it says what the updates are about (their names, versions and sources
are sent, nothing else). Snap, Flatpak and firmware ask their own servers what is new.</p>
<p><b>How to Install</b>: choose <b>File → Ask Claude: How to Install…</b>, or find it in the command palette, and
name a program: "mysql server", "docker", "node 22". Claude answers in the preview pane with the best way to install
it on this computer: which edition and from where, the commands step by step, how to check that it works and how to
remove it again. When you name a version, that is the one it gives; otherwise it recommends one and says why. So that
the advice is about this system and not from memory, Claude is told the system's name and version, its processor type,
the desktop and which package tools it has, and it can ask whether a package is installed and which version the
system's sources offer (that look-up only reads). <b>It only advises: Argonaut runs none of the commands.</b> Read each
one before you type it. The field under the answer asks about another program.</p>
<p><b>Network Connections</b>: choose <b>View → Network Connections</b>, or find it in the command palette. The
preview pane shows what this computer has to do with the network now, made again every three seconds: the network
interfaces with their address and speed (and the Wi-Fi signal), the gateway and the DNS servers; then the programs
that <b>listen</b> for connections, each with its port and what such a port is usually for; then the programs that
are <b>connected</b>, with how many connections and where to. A port that other machines can connect to is in orange
and marked <i>open to the network</i>; one marked <i>this computer only</i> can't be reached from outside. Open to the
network is not a fault by itself: file sharing, printing and finding devices need it, and a firewall may still be in
the way. The <b>ⓘ</b> beside a row says more: every address a program is connected to, or the processes behind a
port. The field above the list searches programs, ports and addresses. It only shows; nothing is closed or blocked.
Two things the system tells only the administrator, so they aren't here: how much each program sends and receives, and
the names of the system's and other users' programs (shown as <i>system or another user</i>; <code>sudo ss -tulnp</code>
in a terminal names them). <b>Analyze with Claude</b> under the list asks Claude what the open ports and the
connections are and whether any deserves a look.</p>
<p><b>System Journal</b>: choose <b>View → System Journal</b>, or find it in the command palette. The preview pane
lists the errors and warnings that the system and its programs have written to the journal since the computer
started: errors first, in red, then the warnings, newest first. A journal repeats itself, so the same message from
the same program is one row, with how often it came (×12) and when last. Each row has who wrote it (<i>kernel</i> is
the system itself), its level, and the message under it; the <b>ⓘ</b> shows the whole message and the service it
belongs to. A few kernel messages are shown as errors whatever they are filed as, because they matter: running out of
memory, a disk's or a file system's errors. Search by who wrote it, by words of the message, or by level ("error").
It updates every ten seconds and only reads. If your user may not read the system's journal (it isn't in the
<i>adm</i> or <i>systemd-journal</i> group), only your own session's messages are listed, and the heading says so.</p>
<p>Under the list is <b>Analyze with Claude</b>, as under the services and the processes: most of a journal is
harmless noise, and Claude says what, if anything, deserves attention (hardware and data first), what it means in
plain words, a command that only looks closer, and what is safe to ignore. Type a question in the field first
("why did chrome close?") and it answers that. The list stays; the report is in the part of the pane under it.
<b>The messages are sent to Anthropic as they are</b>, for the 150 most important kinds of message: they can name
users, computers, network addresses and files, so look at the list before you press the button.</p>
<p><b>Running Processes</b>: choose <b>View → Running Processes</b>, or find it in the command palette. The preview
pane lists every process that is running now, the one using the most memory first, with how it was
started, its process ID in a column of its own and, when it isn't yours, whose it is. It updates every three seconds, scrolls, and only shows: nothing is
stopped. The memory is what a process has in RAM now; memory that several processes share is counted for each of
them, so the rows add up to more than the memory in use, which the bar at the top shows. Beside each name is an
<b>ⓘ</b>: rest the mouse on it, or click it, and a tooltip shows how the process was started, in full, with its
process ID, whose it is and which process started it.</p>
<p>Under the list is <b>Analyze with Claude</b>, with a field at its left, exactly as under the services: Claude
reads the list as it is now and reports the overall picture, anything worth a closer look and why (a program running
from an unusual folder, a name that imitates a well-known program, a name that doesn't match its program) with a
command that only inspects it, and what the processes belong to. Type a question in the field first and Claude
answers that. The list stays; the waiting and then the report are in the part of the pane under it. Claude is sent
each process's name, ID, the ID of the process that started it, whose it is, its memory and the program that was
started, for the 400 biggest; <b>never how a process was started</b> (its arguments can hold passwords). Nothing is
ever stopped or changed.</p>
<p>Both lists can be searched: click the field over the list and type a part of a name (for a process also its
number, its owner or a word of how it was started). The list keeps to what fits and still updates. [[Esc]] empties
the field; [[Esc]] again, or [[Enter]], gives the keys back to the files.</p>
<p>An extension can also supply the preview ([[Space]]) for kinds of files Argonaut can't show itself.
Document Preview does that for Markdown (shown as a formatted page), for HTML files (a page to read: the text
and its structure, without styles, scripts and pictures) and for CSV files and spreadsheets
(.xlsx, .ods: the first sheet as a table).</p>
<p>A command from an extension is carried out by the extension, not by Argonaut, so [[Ctrl+Z]] doesn't undo
it.</p>
<h2>Switching them on and off</h2>
<p><b>Edit → Extensions</b> ([[Ctrl+Shift+X]]) opens the Extensions window. It lists every extension found, where it came from and whether it is running, with a switch for each. Those that come with
Argonaut are on; any other is <b>off until you switch it on</b>.</p>
<p>An extension is an ordinary program: it can do anything you can do, including reading and changing your
files. Switch on only extensions from people you trust.</p>
<h2>Adding one</h2>
<p>Put the extension's folder in <code>~/.local/share/argonaut/extensions</code> (the window's
<b>Open My Extensions Folder</b> button takes you there; it is empty until you add one, because the extensions
that come with Argonaut are part of the program), then open the Extensions window and
switch it on. To remove it, switch it off and delete its folder.</p>
<p>If an extension stops working, Argonaut carries on without it: its marks disappear and everything else
works as before.</p>
<h2>Writing one</h2>
<p>An extension can be written in any language. The guide is <code>docs/extensions.md</code> in Argonaut's
source, with a complete example.</p>
)"));

#if defined(ARGONAUT_TERMINAL)
    add("terminal", tr("Terminal"), tr(R"(
<p>[[Ctrl+`]] opens a terminal under the files, in the open folder. The terminal stays in its folder while you
move about in the files.</p>
<ul>
<li><b>To bring the terminal to the files:</b> press [[Ctrl+`]] again (or the terminal button in the header) while
it is open. If its shell is in another folder than the one you are looking at, it goes there (a <code>cd</code>)
and stays open. If it is already there, the key closes the panel, as does the ✕ on it. This happens only while
the shell is waiting for a command, so nothing is ever typed into a running program. The <code>cd</code> itself
isn't left in the terminal: you only see the prompt change.</li>
<li>Right-click a folder → <b>Open in Terminal</b> opens it there.</li>
<li>The other way round is [[Ctrl+Up]] in the terminal: after a <code>cd</code> there, it brings the files
to the folder the shell is in (and reads the folder again if they are already there).</li>
<li><b>Several terminals, in tabs:</b> [[Ctrl+Shift+`]] (or <b>+</b> beside the tabs, or View → New Terminal)
starts another shell in the open folder, in a tab of its own; the others go on as they were. Click a tab, or press
[[Ctrl+PgUp]] / [[Ctrl+PgDn]] in a terminal, to go from one to the next; drag a tab to put it elsewhere. The keys
above (and Ctrl+Up) are for the terminal whose tab is chosen.</li>
<li>Shortcuts like [[Ctrl+C]] go to the terminal while it has the focus.</li>
<li>A program that asks for the mouse (Claude Code, vim, htop) gets the wheel and the clicks; hold [[Shift]] to
select text instead.</li>
<li>Typing <code>exit</code> closes that terminal's tab, as does the × on the tab (it asks first when a program is
still running in it). When the last one is closed, the panel closes; opening it again starts a new shell.</li>
</ul>
)"));
#endif

    add("textmode", tr("Argonaut in a Terminal"), tr(R"(
<p>Argonaut also runs without a window, inside a terminal: for a server reached over SSH, or for working from
the keyboard. Start it with <code>argonaut --tui</code> (or <code>argonaut-tui</code>), optionally followed by a
folder, or by two folders to start with two panes. It uses the same engine as this window: copying and moving
show progress and can be undone, deleting goes to the Trash, anything permanent is asked about first, and Git
repositories show their marks and branch.</p>
<ul>
<li><b>Moving:</b> the arrow keys, [[PgUp]] / [[PgDn]], [[Home]] / [[End]]. [[Enter]] opens a folder or a file
(with its app; where there is no desktop, a text file is shown in the terminal). [[Backspace]], or Enter on
<code>..</code>, goes up. [[Ctrl+L]] goes to a folder you type.</li>
<li><b>Typing goes to a name</b>, as it does here: type the first letters and the cursor goes to the item that
starts with them (or has them inside). The same letter again goes to the next name that starts with it.
[[Backspace]] takes a letter back, [[Esc]] or a second's pause ends it. So plain letters are never commands.</li>
<li><b>Places:</b> the same list as this window's sidebar is at the left (Recent, Starred, Home and the other
standard folders, the Trash, bookmarks, drives). Click one, or go to the list with [[Tab]], choose with the arrows and
press [[Enter]]; it opens in the pane you were in. [[Ctrl+B]] hides or shows the list.</li>
<li><b>Two panes:</b> [[F4]] shows a second pane beside the first, or goes back to one. [[Tab]] changes pane;
[[F5]] copies and [[F6]] moves to the other pane.</li>
<li><b>List, grid or big icons:</b> [[F9]] goes round three views of the pane: a list with sizes and dates, a
grid of names, and big icons (a picture made of characters for every item, with its name under it and a frame
round the one you are on).</li>
<li><b>A terminal under the files:</b> [[F12]] (or [[Ctrl+`]]) opens a shell under the files (about a third of the height), in the pane's
folder. The same key goes back to the files and leaves the shell open, and from the files it goes to the shell
again, <b>which comes to the folder you are looking at</b> if it was elsewhere (only while it waits for a command);
in the shell, [[Ctrl+Up]] brings the files to its folder and <code>exit</code> closes it. What scrolls off its top is kept (the last 5,000 lines): the
wheel over the panel, its scrollbar at the right, or [[Shift+PgUp]] look back, and typing shows the newest again.</li>
<li><b>Copy and paste:</b> [[Ctrl+C]] copies and [[Ctrl+X]] cuts the selection, as here; go to any folder and
[[Ctrl+V]] pastes. A cut item is faint until it is pasted, and [[Esc]] forgets the cut. Where there is a desktop
and <code>xclip</code> (or <code>wl-clipboard</code> on Wayland) is installed, the clipboard is the desktop's: what
is copied there can be pasted in this window or another file manager, and the other way round. Otherwise (over
SSH) it is Argonaut's own.</li>
<li><b>Files:</b> [[Space]] or [[Insert]] selects, [[F7]] makes a folder, [[F2]] renames, [[F8]] or [[Delete]]
moves to the Trash, [[Shift+Delete]] deletes for good (it asks, and only <b>y</b> means yes), [[Ctrl+Z]] and
[[Ctrl+Y]] undo and redo.</li>
<li><b>Preview:</b> [[F3]] opens a pane at the right that shows what is in the item under the cursor: the lines of a
text file, what is inside an archive, a rough picture of an image. It follows the cursor. [[Tab]] goes to it, where
the arrows and [[PgUp]] / [[PgDn]] scroll; the wheel and its scrollbar work too.</li>
<li><b>Menus:</b> the top line has <b>File</b>, <b>Edit</b>, <b>View</b>, <b>Go</b> and <b>Help</b>, with what this
window's menus have (there are no tabs in a terminal). Click one, or press [[Alt+F]], [[Alt+E]], [[Alt+V]], [[Alt+G]],
[[Alt+H]]; then the arrows move, [[Enter]] chooses and [[Esc]] closes. What can't be done now is dim. Every item shows
its key.</li>
<li><b>Command palette:</b> [[Ctrl+P]] opens a list of everything you can do. Type a few letters ("new", "trash",
"big"), choose with the arrows and press [[Enter]]. Each command shows the key that does the same. Commands that
extensions add to this window's menus are there too, named <b>ext::…</b>, and so are the themes ("theme forest"; the terminal's own
colours are the default): <b>System Dashboard</b> opens in the preview at the right,
drawn with characters, and updates as it does here, <b>Space Overview</b> shows how big the folder is and
the biggest things inside it, <b>Running Services</b> and <b>Running Processes</b> list every running service and process, most memory
first, and <b>System Journal</b> lists the errors and warnings since the system started ([[Tab]] to the preview, then [[/]] searches a list).</li>
<li><b>Connect to Server and Organize with Claude</b> work there too. What an extension asks (a login, your
wishes) is a box over the files: [[Tab]] or the arrows go round its fields and buttons, [[Space]] changes a tick
or a choice, [[Enter]] goes on and [[Esc]] cancels; a password is shown as dots. What it proposes (Claude's plan)
is a list with a tick for every change: [[Space]] ticks, [[a]] is all or none, [[Enter]] applies what is ticked
(and [[Ctrl+Z]] undoes it), [[Esc]] changes nothing. While Claude is being asked the message line says so, and
[[Esc]] stops waiting. On a machine without this window, <b>Edit → Claude API Key…</b> is where the key is typed
(as dots); it is kept as it is here and no extension ever sees it.</li>
<li><b>Effects</b> (View → Effects, on unless you switch them off): a theme in its exact colours where the terminal
can show any colour, names in the colour of their kind (pictures, archives, code), the row under the mouse lit up,
menus with rounded corners and a shadow over a dimmed screen, and scrollbars and bars that move by eighths of a
character. Switch them off for the plain look, or on a slow connection: they send more.</li>
<li><b>Mouse:</b> click, double-click, the wheel, and the words on the bottom line. A right-click works as it does
here: the item becomes the selection (one of several selected items keeps the others) and a menu opens at the
pointer with what can be done with it, extensions' commands included; on empty space or on <b>..</b> the menu is
the folder's. [[Space]] or [[Insert]] select several, and so does the mouse, as here: <b>drag with the button
held</b> and everything the rectangle covers is selected (held past the top or the bottom, the list scrolls on);
[[Ctrl]] and a click adds or removes one item, [[Ctrl]] and a drag adds to what is selected, and a click on empty
space clears the selection.</li>
<li>[[F1]] lists every key; [[Ctrl+Q]] quits ([[F10]] too, where the terminal doesn't keep it for its own menu).</li>
</ul>
<p>Menus, search, tabs and the preview pane are not in the terminal version yet.</p>
)"));

    QString prefs = tr(R"(
<h2>Themes</h2>
<p><b>Edit → Preferences</b> ([[Ctrl+,]]) has seven built-in themes: Follow System, Light, Dark, Midnight Blue,
Rosé Dawn, Forest and Sunset. They are also in <b>View → Theme</b> and in the command palette
([[Ctrl+Shift+P]], then the theme's name).</p>
<p><b>Your own themes.</b> A theme is a small file of colours in the themes folder
(<code>~/.local/share/argonaut/themes</code>), and every file there is offered beside the built-in ones. To make
one, pick the theme closest to what you want and press <b>Save This Theme as a File…</b> in Preferences: the file
is written with every colour spelled out and shown to you. Change a colour in a text editor and save; the window
follows at once. <b>Open Themes Folder</b> shows where they are. A theme is only colours, so one from somebody
else is safe to try.</p>
<h2>Thumbnails</h2>
<p>Previews for PDFs, videos and more are made by the thumbnailers installed on your system, in a sandbox
with no network access. By default they're made only for files on the computer's own disks, because
reading files over USB or the network is slow; Preferences can change that.</p>
)");
#if defined(Q_OS_LINUX)
    prefs += tr(R"(
<h2>Default file manager</h2>
<p>Preferences can make Argonaut open your folders everywhere: from other apps, “Show in folder” in browsers,
and the Super+E key. The same button switches back to GNOME Files.</p>
)");
#endif
#if defined(ARGONAUT_DBUS)
    prefs += tr(R"(
<h2>Open and Save windows</h2>
<p>When an app such as Firefox, Signal or Chrome opens or saves a file, it asks the desktop for a window
to pick it. Preferences → <b>Use Argonaut for Open and Save</b> makes that window Argonaut, with your
sidebar, bookmarks, search and views. Pick a file and press <b>Open</b> (or double-click it); to save,
type a name and press <b>Save</b>; Argonaut asks before replacing a file that's already there.
The app's own options (a file type, an encoding…) are at the bottom. [[Esc]] cancels.</p>
<p>It needs the installed package. The same button switches back to the desktop's own windows.</p>
)");
#endif
    add("prefs", tr("Preferences"), prefs);

    add("shortcuts", tr("Keyboard Shortcuts"), QString()); // made from the actions
}
