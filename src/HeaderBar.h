#pragma once

#include <QPoint>
#include <QWidget>

class QHBoxLayout;

// A title-bar-like strip: dragging empty space moves the window and
// double-clicking toggles maximise, like a GNOME header bar.
class HeaderBar : public QWidget
{
    Q_OBJECT

public:
    static constexpr int Height = 46;

    explicit HeaderBar(QWidget *parent = nullptr);
    QHBoxLayout *row() const { return m_row; }

protected:
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;

private:
    QHBoxLayout *m_row;
    QPoint m_pressPos;
    bool m_pressed = false;
};
