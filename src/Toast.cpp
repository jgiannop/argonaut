#include "Toast.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QTimer>
#include <QToolButton>

#include <utility>

namespace {
constexpr int TimeoutMs = 5000;
constexpr int BottomMargin = 56; // above the floating bar
constexpr int Gap = 8;
} // namespace

Toast::Toast(QWidget *parent)
    : QFrame(parent)
    , m_label(new QLabel(this))
    , m_action(new QToolButton(this))
    , m_close(new QToolButton(this))
    , m_timer(new QTimer(this))
{
    setObjectName("toast");
    auto *row = new QHBoxLayout(this);
    row->setContentsMargins(18, 6, 6, 6);
    row->setSpacing(10);

    m_label->setObjectName("toastText");
    m_action->setObjectName("toastAction");
    m_action->setFocusPolicy(Qt::NoFocus);
    m_close->setObjectName("flat");
    m_close->setFocusPolicy(Qt::NoFocus);
    m_close->setIconSize(QSize(16, 16));
    m_close->setFixedSize(28, 28);
    m_close->setToolTip(tr("Dismiss"));

    row->addWidget(m_label, 1);
    row->addWidget(m_action);
    row->addWidget(m_close);

    m_timer->setSingleShot(true);
    m_timer->setInterval(TimeoutMs);
    connect(m_timer, &QTimer::timeout, this, &QWidget::hide);
    connect(m_close, &QToolButton::clicked, this, &QWidget::hide);
    connect(m_action, &QToolButton::clicked, this, [this] {
        hide();
        if (auto callback = std::exchange(m_callback, {}))
            callback();
    });
    hide();
}

void Toast::showMessage(const QString &text, const QString &actionText,
                        std::function<void()> action)
{
    m_label->setText(text);
    m_action->setText(actionText);
    m_action->setVisible(!actionText.isEmpty() && action);
    m_callback = std::move(action);
    adjustSize();
    reposition();
    show();
    raise();
    m_timer->start();
}

void Toast::setTheme(const Theme &theme)
{
    m_close->setIcon(glyphIcon(Glyph::Close, theme.fg));
}

void Toast::setAvoidRect(const QRect &rect)
{
    m_avoid = rect;
    if (isVisible())
        reposition();
}

void Toast::reposition()
{
    QWidget *host = parentWidget();
    if (!host)
        return;
    const int width = qMin(sizeHint().width(), host->width() - 20);
    resize(width, sizeHint().height());
    QRect target((host->width() - width) / 2, host->height() - height() - BottomMargin, width,
                 height());
    if (m_avoid.isValid() && target.intersects(m_avoid.adjusted(-Gap, -Gap, Gap, Gap)))
        target.moveBottom(m_avoid.top() - Gap);
    move(target.topLeft());
}
