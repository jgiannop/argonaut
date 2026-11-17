#pragma once

#include "Extensions.h"

#include <QEvent>
#include <QHelpEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QToolTip>

// A picture an extension drew, shown at its own size, with the parts of it
// that have something to say (Extensions::Tip: the ⓘ beside a service): the
// mouse resting on one, or pressing it, shows its text as a tooltip, and the
// pointer is a hand over it. The text is the extension's, so it is shown as
// plain text, never as rich text.
class TipPicture : public QLabel
{
public:
    explicit TipPicture(QWidget *parent = nullptr) : QLabel(parent) { setMouseTracking(true); }

    // `scale`: picture pixels to a point of the widget (the screen's ratio).
    void setTips(const QList<Extensions::Tip> &tips, double scale)
    {
        m_tips = tips;
        m_scale = scale > 0 ? scale : 1.0;
        if (m_tips.isEmpty())
            unsetCursor();
    }
    int tipCount() const { return int(m_tips.size()); }
    // The text of the part at this point of the widget ("" : none there).
    QString tipAt(const QPoint &point) const
    {
        const QPoint inPicture(int(point.x() * m_scale), int(point.y() * m_scale));
        for (const Extensions::Tip &tip : m_tips) {
            if (tip.area.contains(inPicture))
                return tip.text;
        }
        return {};
    }
    // Where a part is, in the widget (tests press there).
    QRect tipArea(int index) const
    {
        const QRect a = m_tips.value(index).area;
        return QRect(int(a.x() / m_scale), int(a.y() / m_scale), qMax(1, int(a.width() / m_scale)), qMax(1, int(a.height() / m_scale)));
    }
    QString shownTip() const { return m_shown; } // what was last shown (tests)

protected:
    bool event(QEvent *event) override
    {
        if (event->type() == QEvent::ToolTip) {
            auto *help = static_cast<QHelpEvent *>(event);
            show(tipAt(help->pos()), help->globalPos());
            return true;
        }
        return QLabel::event(event);
    }
    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (tipAt(event->pos()).isEmpty())
            unsetCursor();
        else
            setCursor(Qt::PointingHandCursor);
        QLabel::mouseMoveEvent(event);
    }
    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton)
            show(tipAt(event->pos()), event->globalPosition().toPoint()); // a press shows it at once
        QLabel::mousePressEvent(event);
    }

private:
    void show(const QString &text, const QPoint &globalPos)
    {
        m_shown = text;
        if (text.isEmpty()) {
            QToolTip::hideText();
            return;
        }
        // Escaped inside <qt>: the tooltip wraps long lines and shows the text as it is.
        QToolTip::showText(globalPos, QStringLiteral("<qt>") + text.toHtmlEscaped().replace(QLatin1Char('\n'), QStringLiteral("<br>")) + QStringLiteral("</qt>"),
                           this);
    }

    QList<Extensions::Tip> m_tips;
    double m_scale = 1.0;
    QString m_shown;
};
