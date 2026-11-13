#include "HeaderBar.h"

#include <QApplication>
#include <QHBoxLayout>
#include <QMouseEvent>
#include <QWindow>

HeaderBar::HeaderBar(QWidget *parent)
    : QWidget(parent)
    , m_row(new QHBoxLayout(this))
{
    setFixedHeight(Height);
    m_row->setContentsMargins(7, 7, 7, 7);
    m_row->setSpacing(6);
}

void HeaderBar::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        m_pressed = true;
        m_pressPos = event->position().toPoint();
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

void HeaderBar::mouseMoveEvent(QMouseEvent *event)
{
    // Start the move only after a real drag, so double-click still works.
    if (m_pressed && (event->buttons() & Qt::LeftButton)
        && (event->position().toPoint() - m_pressPos).manhattanLength()
               >= QApplication::startDragDistance()) {
        m_pressed = false;
        if (QWindow *handle = window()->windowHandle())
            handle->startSystemMove();
        event->accept();
        return;
    }
    QWidget::mouseMoveEvent(event);
}

void HeaderBar::mouseReleaseEvent(QMouseEvent *event)
{
    m_pressed = false;
    QWidget::mouseReleaseEvent(event);
}

void HeaderBar::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton) {
        QWidget::mouseDoubleClickEvent(event);
        return;
    }
    m_pressed = false;
    QWidget *w = window();
    if (w->isMaximized())
        w->showNormal();
    else
        w->showMaximized();
    event->accept();
}
