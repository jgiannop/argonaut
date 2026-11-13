#pragma once

#include "Theme.h"

#include <QFrame>
#include <functional>

class QLabel;
class QTimer;
class QToolButton;

// libadwaita-style toast: a rounded message at the bottom centre of its
// parent with an optional action button (e.g. "Undo"). Hides after a while.
class Toast : public QFrame
{
    Q_OBJECT

public:
    explicit Toast(QWidget *parent);

    void showMessage(const QString &text, const QString &actionText = QString(),
                     std::function<void()> action = {});
    void setTheme(const Theme &theme);
    // Area (in parent coordinates) to stay clear of, e.g. the progress card.
    void setAvoidRect(const QRect &rect);
    void reposition();

private:
    QLabel *m_label;
    QToolButton *m_action;
    QToolButton *m_close;
    QTimer *m_timer;
    std::function<void()> m_callback;
    QRect m_avoid;
};
