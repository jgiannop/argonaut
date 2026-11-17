// What extensions ask and propose, in the terminal: a form (a login, "start
// with these wishes?") and a plan (changes to tick and apply) are boxes over
// the files; while Claude is asked something the message line says so. A
// part of Tui::App in a file of its own, as the menus and the preview are.

#include "AppInternal.h"
#include "Frames.h"

#include "FileOperations.h"
#include "Format.h"
#include "KeyStore.h"
#include "PlanApply.h"

#include <QRegularExpression>

namespace Tui {

namespace {

// Prose as the lines it takes in `width` cells: broken at spaces, a word
// longer than a line cut where it must be.
QStringList wrapped(const QString &text, int width)
{
    QStringList out;
    width = qMax(4, width);
    for (const QString &paragraph : text.split(QLatin1Char('\n'))) {
        QString line;
        for (QString word : paragraph.split(QLatin1Char(' '), Qt::SkipEmptyParts)) {
            while (Screen::widthOf(word) > width) { // a word that fits no line
                if (!line.isEmpty()) {
                    out.append(line);
                    line.clear();
                }
                qsizetype take = word.size();
                while (take > 1 && Screen::widthOf(word.left(take)) > width)
                    --take;
                out.append(word.left(take));
                word = word.mid(take);
            }
            if (line.isEmpty())
                line = word;
            else if (Screen::widthOf(line) + 1 + Screen::widthOf(word) <= width)
                line += QLatin1Char(' ') + word;
            else {
                out.append(line);
                line = word;
            }
        }
        out.append(line); // an empty paragraph is an empty line
    }
    while (!out.isEmpty() && out.last().isEmpty())
        out.removeLast();
    return out;
}

// A report is Markdown; here it is read as text: headings without their
// marks (and bold), the rest as written, pictures and link targets left out
// (nothing is ever fetched for an extension).
struct ReportLine
{
    QString text;
    bool heading = false;
};

QList<ReportLine> reportLines(const QString &markdown, int width)
{
    static const QRegularExpression picture(QStringLiteral("!\\[([^\\]]*)\\]\\([^)]*\\)"));
    static const QRegularExpression link(QStringLiteral("\\[([^\\]]*)\\]\\([^)]*\\)"));
    QList<ReportLine> out;
    for (QString line : markdown.split(QLatin1Char('\n'))) {
        line.replace(picture, QStringLiteral("\\1"));
        line.replace(link, QStringLiteral("\\1"));
        line.remove(QStringLiteral("**"));
        line.remove(QLatin1Char('`'));
        bool heading = false;
        if (line.startsWith(QLatin1Char('#'))) {
            heading = true;
            while (line.startsWith(QLatin1Char('#')))
                line.remove(0, 1);
        }
        line = line.trimmed();
        if (line.isEmpty()) {
            if (!out.isEmpty() && !out.last().text.isEmpty())
                out.append(ReportLine{});
            continue;
        }
        for (const QString &part : wrapped(line, width))
            out.append({part, heading});
    }
    while (!out.isEmpty() && out.last().text.isEmpty())
        out.removeLast();
    return out;
}

// A box over the files: the rest steps back, a frame with a title.
void drawBox(Screen &screen, int x, int y, int width, int height, const QString &title, bool effects)
{
    const Style box{Ink::Normal, Paper::Bar, false};
    const Style edge{Ink::Accent, Paper::Bar, false};
    if (effects) {
        screen.dim(0, 0, screen.width(), screen.height());
        screen.shade(x + 2, y + height, width, 1);
        screen.shade(x + width, y + 1, 2, height - 1);
    }
    screen.fill(x, y, width, height, box);
    screen.put(x, y, frameTop(width, effects), edge);
    screen.put(x, y + height - 1, frameBottom(width, effects), edge);
    for (int row = y + 1; row < y + height - 1; ++row) {
        screen.put(x, row, QStringLiteral("│"), edge);
        screen.put(x + width - 1, row, QStringLiteral("│"), edge);
    }
    screen.put(x + 2, y, QLatin1Char(' ') + Screen::elided(title, width - 6, false) + QLatin1Char(' '), {Ink::Accent, Paper::Bar, true});
}

} // namespace

// A box closed: the keys are the files' again, unless a name clash of a
// running job has been waiting for its turn.
void App::backToNormal()
{
    m_mode = Mode::Normal;
    if (!m_conflicts.isEmpty())
        nextConflict();
    emit changed();
}

// ---------------------------------------------------------------- forms ---

QStringList App::formButtons() const
{
    QStringList buttons = m_form.buttons;
    if (buttons.isEmpty())
        buttons << QStringLiteral("OK");
    buttons << QStringLiteral("Cancel");
    return buttons;
}

// "Connect anonymously" and the like: while such a tick is on, the other
// fields don't count (they are dim and the keys pass them over).
bool App::formFieldOff(int field) const
{
    for (int i = 0; i < m_form.fields.size(); ++i) {
        const Extensions::FormField &f = m_form.fields.at(i);
        if (i != field && f.kind == Extensions::FormField::Check && f.alone && m_formValues.value(i) == QLatin1String("true"))
            return true;
    }
    return false;
}

QList<int> App::formStops() const
{
    QList<int> stops;
    for (int i = 0; i < m_form.fields.size(); ++i) {
        if (!formFieldOff(i))
            stops << i;
    }
    for (int b = 0; b < formButtons().size(); ++b)
        stops << -1 - b;
    return stops;
}

void App::showForm(const Extensions::Form &form)
{
    m_form = form;
    m_formValues.clear();
    for (const Extensions::FormField &field : form.fields) {
        QString value = field.value;
        if (field.kind == Extensions::FormField::Check)
            value = value == QLatin1String("true") ? value : QStringLiteral("false");
        else if (field.kind == Extensions::FormField::Choice && !field.choices.contains(value))
            value = field.choices.value(0);
        m_formValues << value;
    }
    m_formStop = 0;
    m_formCaret = int(m_formValues.value(formStops().value(0)).size());
    m_formSpots.clear();
    m_mode = Mode::Form;
    emit changed();
}

void App::closeForm(bool cancelled, int button)
{
    QHash<QString, QString> values;
    for (int i = 0; i < m_form.fields.size(); ++i)
        values.insert(m_form.fields.at(i).id, m_formValues.value(i));
    const Extensions::Form form = m_form;
    m_form = {};
    m_formValues.clear(); // a password isn't kept longer than it is needed
    backToNormal();
    Extensions::Host::instance()->answerForm(form.extension, form.id, cancelled, cancelled ? QHash<QString, QString>() : values, button);
}

void App::formStep(int by)
{
    const QList<int> stops = formStops();
    m_formStop = int((m_formStop + by + stops.size()) % stops.size());
    const int stop = stops.at(m_formStop);
    m_formCaret = stop >= 0 ? int(m_formValues.value(stop).size()) : 0;
}

void App::formKey(const Key &key)
{
    QList<int> stops = formStops();
    m_formStop = qBound(0, m_formStop, int(stops.size()) - 1);
    const int stop = stops.at(m_formStop);
    const int buttons = int(formButtons().size());
    if (key.code == Key::Escape) {
        closeForm(true, 0);
        return;
    }
    if (key.code == Key::Tab || key.code == Key::Down || key.code == Key::Up) {
        formStep(key.code == Key::Up || key.shift ? -1 : 1);
        emit changed();
        return;
    }
    if (stop < 0) { // a button
        const int button = -1 - stop;
        if (key.code == Key::Left || key.code == Key::Right) {
            const int next = button + (key.code == Key::Left ? -1 : 1);
            if (next >= 0 && next < buttons)
                m_formStop = int(stops.indexOf(-1 - next));
        } else if (key.code == Key::Enter || key.isChar(' ')) {
            closeForm(button == buttons - 1, button);
            return;
        }
        emit changed();
        return;
    }
    const Extensions::FormField &field = m_form.fields.at(stop);
    QString &value = m_formValues[stop];
    if (key.code == Key::Enter) {
        // On to the next field; from the last one, the form's first button.
        if (stops.value(m_formStop + 1, -1) >= 0)
            formStep(1);
        else {
            closeForm(false, 0);
            return;
        }
    } else if (field.kind == Extensions::FormField::Check) {
        if (key.isChar(' ') || key.code == Key::Left || key.code == Key::Right) {
            value = value == QLatin1String("true") ? QStringLiteral("false") : QStringLiteral("true");
            // The stops may be others now ("alone"): stay on this tick.
            m_formStop = qMax(0, int(formStops().indexOf(stop)));
        }
    } else if (field.kind == Extensions::FormField::Choice) {
        const int count = int(field.choices.size());
        if (count > 0 && (key.isChar(' ') || key.code == Key::Left || key.code == Key::Right)) {
            const int at = int(field.choices.indexOf(value));
            value = field.choices.at((at + (key.code == Key::Left ? -1 : 1) + count) % count);
        }
    } else { // text, or a secret
        m_formCaret = qBound(0, m_formCaret, int(value.size()));
        switch (key.code) {
        case Key::Left: m_formCaret = qMax(0, m_formCaret - 1); break;
        case Key::Right: m_formCaret = qMin(int(value.size()), m_formCaret + 1); break;
        case Key::Home: m_formCaret = 0; break;
        case Key::End: m_formCaret = int(value.size()); break;
        case Key::Backspace:
            if (m_formCaret > 0)
                value.remove(--m_formCaret, 1);
            break;
        case Key::Delete: value.remove(m_formCaret, 1); break;
        case Key::PageDown:
        case Key::PageUp:
            // What the extension suggests (servers used before), one after another.
            if (field.kind == Extensions::FormField::Text && !field.choices.isEmpty()) {
                const int count = int(field.choices.size());
                const int at = int(field.choices.indexOf(value));
                value = field.choices.at(at < 0 ? 0 : (at + (key.code == Key::PageUp ? -1 : 1) + count) % count);
                m_formCaret = int(value.size());
            }
            break;
        case Key::Char:
            if (key.isControl('u')) {
                value.clear();
                m_formCaret = 0;
            } else if (!key.ctrl && !key.alt && key.ch >= 0x20) {
                const QString typed = QString::fromUcs4(&key.ch, 1);
                value.insert(m_formCaret, typed);
                m_formCaret += int(typed.size());
            }
            break;
        default: break;
        }
    }
    emit changed();
}

// A click on a field goes to it (a tick changes, a choice goes to its next);
// a click on a button presses it. Beside the box: nothing.
void App::mouseInForm(const Mouse &mouse)
{
    if (mouse.kind != Mouse::Press)
        return;
    const QList<int> stops = formStops();
    for (const FormSpot &spot : std::as_const(m_formSpots)) {
        if (mouse.y != spot.y || mouse.x < spot.x || mouse.x >= spot.x + spot.width)
            continue;
        const int at = int(stops.indexOf(spot.stop));
        if (at < 0)
            return;
        m_formStop = at;
        if (spot.stop < 0) {
            key(Key::of(Key::Enter));
            return;
        }
        m_formCaret = int(m_formValues.value(spot.stop).size());
        const Extensions::FormField::Kind kind = m_form.fields.at(spot.stop).kind;
        if (kind == Extensions::FormField::Check || kind == Extensions::FormField::Choice)
            key(Key::character(' '));
        emit changed();
        return;
    }
}

void App::drawForm(Screen &screen)
{
    const int w = screen.width();
    const int h = screen.height();
    const int width = qMin(72, w - 4);
    const int inner = width - 4;
    const int x = (w - width) / 2;
    const Style dim{Ink::Dim, Paper::Bar, false};
    const QList<int> stops = formStops();
    m_formStop = qBound(0, m_formStop, int(stops.size()) - 1);
    const int here = stops.at(m_formStop);

    // What is said above the fields. The fields and buttons always fit; the
    // text gives way where the terminal is low.
    QStringList said;
    if (!m_form.heading.isEmpty())
        said << wrapped(m_form.heading, inner);
    const int headingLines = int(said.size());
    if (!m_form.text.isEmpty()) {
        if (!said.isEmpty())
            said << QString();
        said << wrapped(m_form.text, inner);
    }
    const QStringList warning = m_form.warning.isEmpty() ? QStringList() : wrapped(m_form.warning, inner);
    int fieldLines = 0;
    for (const Extensions::FormField &field : std::as_const(m_form.fields))
        fieldLines += field.kind == Extensions::FormField::Check ? 1 : 2;
    const int fixed = 2 /*frame*/ + int(warning.size()) + (warning.isEmpty() ? 0 : 1) + fieldLines + 2 /*gap, buttons*/;
    const int room = qMax(1, h - 2 - fixed);
    if (said.size() > room) {
        said = said.mid(0, room);
        said.last() = QStringLiteral("…");
    }
    const int height = qMin(h - 1, fixed + int(said.size()) + (said.isEmpty() ? 0 : 1));
    const int top = qMax(0, (h - height) / 2);
    // The frame always says whose form it is.
    const QString title = m_form.title.isEmpty() || m_form.title == m_form.extensionName
                              ? m_form.extensionName
                              : QStringLiteral("%1 — %2").arg(m_form.title, m_form.extensionName);
    drawBox(screen, x, top, width, height, title, m_effects);

    m_formSpots.clear();
    int y = top + 1;
    for (int i = 0; i < said.size(); ++i)
        screen.put(x + 2, y++, said.at(i), {Ink::Normal, Paper::Bar, i < headingLines}, inner);
    if (!said.isEmpty())
        ++y;
    for (const QString &line : warning)
        screen.put(x + 2, y++, line, {Ink::Bad, Paper::Bar, true}, inner);
    if (!warning.isEmpty())
        ++y;
    for (int i = 0; i < m_form.fields.size() && y < top + height - 2; ++i) {
        const Extensions::FormField &field = m_form.fields.at(i);
        const QString value = m_formValues.value(i);
        const bool off = formFieldOff(i);
        const bool focus = i == here;
        const Style label{off ? Ink::Dim : focus ? Ink::Accent : Ink::Normal, Paper::Bar, focus};
        if (field.kind == Extensions::FormField::Check) {
            const QString text = (value == QLatin1String("true") ? QStringLiteral("[x] ") : QStringLiteral("[ ] ")) + field.label;
            if (focus)
                screen.fill(x + 1, y, width - 2, 1, {Ink::Normal, Paper::Cursor, true});
            screen.put(x + 2, y, text, focus ? Style{Ink::Normal, Paper::Cursor, true} : label, inner);
            if (focus)
                screen.setCursor(x + 3, y);
            m_formSpots.append({x + 1, y, width - 2, i});
            ++y;
            continue;
        }
        screen.put(x + 2, y, field.label, label, inner);
        if (field.kind == Extensions::FormField::Text && !field.choices.isEmpty() && focus) {
            const QString hint = QStringLiteral("PgDn: %1 used before").arg(field.choices.size());
            if (Screen::widthOf(field.label) + Screen::widthOf(hint) + 2 <= inner)
                screen.put(x + 2 + inner - Screen::widthOf(hint), y, hint, dim);
        }
        ++y;
        if (field.kind == Extensions::FormField::Choice) {
            const Style style{off ? Ink::Dim : Ink::Normal, focus ? Paper::Cursor : Paper::Field, focus};
            screen.fill(x + 2, y, inner, 1, style);
            const QString shown = field.choices.isEmpty() ? QStringLiteral("(nothing to choose)") : QStringLiteral("‹ %1 ›").arg(value);
            screen.put(x + 3, y, Screen::elided(shown, inner - 2, false), style);
            if (focus)
                screen.setCursor(x + 3, y);
        } else {
            const Style style{off ? Ink::Dim : Ink::Normal, Paper::Field, false};
            screen.fill(x + 2, y, inner, 1, style);
            // A secret is never drawn: a dot a character.
            const QString shown = field.kind == Extensions::FormField::Secret ? QString(value.size(), QChar(0x2022)) : value;
            if (shown.isEmpty() && !focus) {
                screen.put(x + 3, y, Screen::elided(field.placeholder, inner - 2, false), {Ink::Dim, Paper::Field, false});
            } else {
                const int caret = focus ? qBound(0, m_formCaret, int(shown.size())) : 0;
                int skip = 0; // the caret stays in sight in a long text
                while (Screen::widthOf(shown.mid(skip, caret - skip)) > inner - 3)
                    ++skip;
                screen.put(x + 3, y, shown.mid(skip), style, inner - 2);
                if (focus)
                    screen.setCursor(x + 3 + Screen::widthOf(shown.mid(skip, caret - skip)), y);
            }
        }
        m_formSpots.append({x + 2, y, inner, i});
        ++y;
    }
    // The buttons, at the right of the last line inside the frame.
    const QStringList buttons = formButtons();
    int total = 0;
    for (const QString &button : buttons)
        total += Screen::widthOf(button) + 4 + 1;
    int bx = qMax(x + 2, x + width - 2 - (total - 1));
    const int by = top + height - 2;
    for (int b = 0; b < buttons.size(); ++b) {
        const bool focus = here == -1 - b;
        const QString text = QStringLiteral("[ %1 ]").arg(buttons.at(b));
        const int used = screen.put(bx, by, text, {focus ? Ink::Normal : b == 0 ? Ink::Accent : Ink::Normal, focus ? Paper::Cursor : Paper::Bar, focus || b == 0},
                                    x + width - 2 - bx);
        if (focus)
            screen.setCursor(bx + 2, by);
        m_formSpots.append({bx, by, used, -1 - b});
        bx += used + 1;
    }
}

// ---------------------------------------------------------------- plans ---

int App::planKeptCount() const
{
    return int(m_planKept.count(true));
}

void App::showPlan(const Extensions::Plan &plan)
{
    m_plan = plan;
    m_planKept.clear();
    for (const Extensions::PlanMove &move : plan.moves)
        m_planKept << move.problem.isEmpty(); // what can't be done is never ticked
    m_planCursor = 0;
    m_planTop = 0;
    m_mode = Mode::Plan;
    emit changed();
}

// Apply: the kept moves are checked again as the folder is now and done by
// the file engine (PlanApply, the window's code too), so Ctrl+Z undoes them.
// Otherwise the extension hears the plan was dismissed.
void App::closePlan(bool apply)
{
    const Extensions::Plan plan = m_plan;
    QList<Extensions::PlanMove> kept;
    for (int i = 0; i < plan.moves.size(); ++i) {
        if (m_planKept.value(i))
            kept << plan.moves.at(i);
    }
    m_plan = {};
    m_planKept.clear();
    backToNormal();
    if (!apply || kept.isEmpty()) {
        Extensions::Host::instance()->answerPlan(plan.extension, plan.id, true);
        say(QStringLiteral("Nothing was changed"));
        return;
    }
    say(PlanApply::message(PlanApply::run(m_ops, plan, kept)));
}

void App::planKey(const Key &key)
{
    const int count = int(m_plan.moves.size());
    const int page = qMax(1, m_planShown - 1);
    auto go = [&](int to) {
        m_planCursor = count == 0 ? 0 : qBound(0, to, count - 1);
        // The move the keys are on stays in sight.
        const int line = m_planHead + m_planCursor;
        if (line < m_planTop)
            m_planTop = line;
        if (line >= m_planTop + m_planShown)
            m_planTop = line - m_planShown + 1;
    };
    switch (key.code) {
    case Key::Escape:
        closePlan(false);
        return;
    case Key::Enter:
        closePlan(true);
        return;
    case Key::Up:
        // Above the first move is the report: the lines move, not the cursor.
        if (m_planCursor > 0)
            go(m_planCursor - 1);
        else
            m_planTop = qMax(0, m_planTop - 1);
        break;
    case Key::Down: go(m_planCursor + 1); break;
    case Key::PageUp:
        if (m_planCursor == 0)
            m_planTop = qMax(0, m_planTop - page);
        else
            go(m_planCursor - page);
        break;
    case Key::PageDown: go(m_planCursor + page); break;
    case Key::Home:
        go(0);
        m_planTop = 0;
        break;
    case Key::End:
        go(count - 1);
        m_planTop = qMax(0, m_planTotal - m_planShown);
        break;
    case Key::Char:
        if (key.isChar(' ') && m_planCursor < count) {
            if (m_plan.moves.at(m_planCursor).problem.isEmpty())
                m_planKept[m_planCursor] = !m_planKept.at(m_planCursor);
            go(m_planCursor + 1);
        } else if (key.isChar('a') || key.isChar('A')) {
            // All that can be done, or none when all of those are ticked already.
            int possible = 0;
            for (const Extensions::PlanMove &move : std::as_const(m_plan.moves))
                possible += move.problem.isEmpty() ? 1 : 0;
            const bool all = planKeptCount() < possible;
            for (int i = 0; i < count; ++i)
                m_planKept[i] = all && m_plan.moves.at(i).problem.isEmpty();
        }
        break;
    default: break;
    }
    emit changed();
}

// The wheel moves through it; a click on a move puts the keys there and
// changes its tick. Applying is the keyboard's (Enter).
void App::mouseInPlan(const Mouse &mouse)
{
    if (mouse.isWheel()) {
        m_planTop = qBound(0, m_planTop + (mouse.kind == Mouse::WheelUp ? -3 : 3), qMax(0, m_planTotal - m_planShown));
        emit changed();
        return;
    }
    if (mouse.kind != Mouse::Press || mouse.x <= m_planX || mouse.x >= m_planX + m_planWidth - 1)
        return;
    const int line = m_planTop + mouse.y - m_planY;
    const int move = line - m_planHead;
    if (mouse.y < m_planY || mouse.y >= m_planY + m_planShown || move < 0 || move >= m_plan.moves.size())
        return;
    m_planCursor = move;
    if (m_plan.moves.at(move).problem.isEmpty())
        m_planKept[move] = !m_planKept.at(move);
    emit changed();
}

void App::drawPlan(Screen &screen)
{
    const int w = screen.width();
    const int h = screen.height();
    const int width = qMin(110, w - 4);
    const int inner = width - 4;
    const int x = (w - width) / 2;
    const Style dim{Ink::Dim, Paper::Bar, false};
    const QString title = m_plan.title.isEmpty() || m_plan.title == m_plan.extensionName
                              ? m_plan.extensionName
                              : QStringLiteral("%1 — %2").arg(m_plan.title, m_plan.extensionName);

    // The report, a line of what is below it, then a line a move.
    QList<ReportLine> head = reportLines(m_plan.report, inner);
    if (!head.isEmpty())
        head.append(ReportLine{});
    const int count = int(m_plan.moves.size());
    head.append({count == 0 ? QStringLiteral("Nothing to change.")
                            : QStringLiteral("In %1 (%2 of %3 ticked):").arg(tildePath(m_plan.folder)).arg(planKeptCount()).arg(count),
                 true});
    m_planHead = int(head.size());
    const QStringList note = m_plan.note.isEmpty() ? QStringList() : wrapped(m_plan.note, inner);
    m_planTotal = m_planHead + count + (note.isEmpty() ? 0 : 1 + int(note.size()));
    // As tall as what it holds, as far as the screen goes (the message and
    // key lines stay in sight).
    const int height = qBound(3, m_planTotal + 2, h - 3);
    const int top = qMax(1, (h - 2 - height) / 2);
    drawBox(screen, x, top, width, height, title, m_effects);
    m_planX = x;
    m_planWidth = width;
    m_planY = top + 1;
    m_planShown = qMax(1, height - 2);
    m_planTop = qBound(0, m_planTop, qMax(0, m_planTotal - m_planShown));
    m_planCursor = count == 0 ? 0 : qBound(0, m_planCursor, count - 1);

    for (int row = 0; row < m_planShown; ++row) {
        const int line = m_planTop + row;
        const int y = m_planY + row;
        if (line >= m_planTotal)
            break;
        if (line < m_planHead) {
            screen.put(x + 2, y, head.at(line).text, {Ink::Normal, Paper::Bar, head.at(line).heading}, inner);
            continue;
        }
        const int at = line - m_planHead;
        if (at >= count) { // the note, after an empty line
            if (at > count)
                screen.put(x + 2, y, note.value(at - count - 1), dim, inner);
            continue;
        }
        const Extensions::PlanMove &move = m_plan.moves.at(at);
        const bool cursor = at == m_planCursor;
        const bool bad = !move.problem.isEmpty();
        const Paper paper = cursor ? Paper::Cursor : Paper::Bar;
        if (cursor) {
            screen.fill(x + 1, y, width - 2, 1, {Ink::Normal, paper, true});
            screen.setCursor(x + 3, y);
        }
        const Style main{bad ? Ink::Bad : Ink::Normal, paper, cursor};
        // "[x] name → Folder/new name" (or "→ Trash", "→ into Photos.zip"), then why (or why not), as far as there is room.
        const QString where = move.becomes();
        const QString tick = bad ? QStringLiteral("[!] ") : m_planKept.value(at) ? QStringLiteral("[x] ") : QStringLiteral("[ ] ");
        int cx = x + 2;
        cx += screen.put(cx, y, tick, main);
        const int nameRoom = qMax(8, (inner - 4) * 2 / 5);
        cx += screen.put(cx, y, Screen::elided(move.what(), nameRoom), main);
        if (!where.isEmpty()) {
            cx += screen.put(cx, y, QStringLiteral(" → "), {bad ? Ink::Bad : Ink::Dim, paper, false});
            // What goes to the Trash is said in the colour of a warning: it is the one step that takes something away.
            const Ink ink = bad ? Ink::Bad : move.action == QLatin1String("trash") ? Ink::Warning : Ink::Folder;
            cx += screen.put(cx, y, Screen::elided(where, qMax(8, (inner - 4) * 2 / 5)), {ink, paper, cursor});
        }
        const QString why = bad ? move.problem : move.reason;
        const int left = x + 2 + inner - cx - 2;
        if (!why.isEmpty() && left >= 8)
            screen.put(cx + 2, y, Screen::elided(why, left, false), {bad ? Ink::Bad : cursor ? Ink::Normal : Ink::Dim, paper, false});
    }
    // More than fits: a handle on the right edge.
    if (m_planTotal > m_planShown) {
        const int thumb = qBound(1, m_planShown * m_planShown / m_planTotal, m_planShown - 1);
        const int at = (m_planShown - thumb) * m_planTop / (m_planTotal - m_planShown);
        for (int i = at; i < at + thumb; ++i)
            screen.put(x + width - 1, m_planY + i, QStringLiteral("█"), {Ink::Accent, Paper::Bar, false});
    }
}

// --------------------------------------------------------------- Claude ---

// Edit → Claude API Key…: the one place it is typed, for every extension
// that asks Claude through Argonaut. It is typed as dots, goes to where
// Argonaut keeps it (the keyring, or a file only the user can read) and
// never to an extension.
void App::setClaudeKey()
{
    KeyStore::Where where = KeyStore::Where::None;
    const bool has = !KeyStore::load(&where).isEmpty();
    if (where == KeyStore::Where::Environment) {
        say(QStringLiteral("The key is taken from ANTHROPIC_API_KEY in the environment, which comes first"));
        return;
    }
    prompt(has ? QStringLiteral("New Claude API key (now in %1):").arg(KeyStore::describe(where)) : QStringLiteral("Claude API key:"), QString(), -1,
           [this](const QString &typed) {
               KeyStore::Where saved = KeyStore::Where::None;
               QString error;
               if (KeyStore::save(typed.trimmed(), &saved, &error))
                   say(QStringLiteral("The key is kept in %1").arg(KeyStore::describe(saved)));
               else
                   say(QStringLiteral("The key couldn't be kept: %1").arg(error));
           });
    m_promptSecret = true;
}

} // namespace Tui
