#include "FadeOverlay.h"

#include "Tools.h"

#include <QCoreApplication>
#include <QBackingStore>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QImage>
#include <QPainter>
#include <QProcess>
#include <QSettings>
#include <QVariantAnimation>

#include <cstdio>

namespace {

// GNOME's "reduce animation" switch (enable-animations). Asked once, in the
// background; animations are assumed on until the answer says otherwise.
bool desktopAnimations()
{
    static bool allowed = true;
#if defined(Q_OS_LINUX)
    static bool asked = false;
    if (!asked) {
        asked = true;
        auto *process = new QProcess(qApp);
        QObject::connect(process, &QProcess::finished, process, [process] {
            if (process->readAllStandardOutput().trimmed() == "false")
                allowed = false;
            process->deleteLater();
        });
        QObject::connect(process, &QProcess::errorOccurred, process, &QObject::deleteLater);
        process->start(Tools::program("ARGONAUT_GSETTINGS_PROGRAM", QStringLiteral("gsettings")),
                       {QStringLiteral("get"), QStringLiteral("org.gnome.desktop.interface"),
                        QStringLiteral("enable-animations")});
    }
#endif
    return allowed;
}

// The view as it is on screen right now. Copied out of the window's own
// buffer where that holds the last frame (X11): a plain memory copy, many
// times cheaper on a big window than painting the view once more
// (QWidget::grab), which is the fallback.
QPixmap pictureOf(QWidget *view)
{
    static const bool bufferIsLastFrame = QGuiApplication::platformName() == QLatin1String("xcb");
    QWidget *top = view->window();
    QBackingStore *store = bufferIsLastFrame && top ? top->backingStore() : nullptr;
    QPaintDevice *device = store ? store->paintDevice() : nullptr;
    if (device && device->devType() == QInternal::Image) {
        const QImage *buffer = static_cast<QImage *>(device);
        const qreal ratio = buffer->devicePixelRatio();
        const QRect area(view->mapTo(top, QPoint(0, 0)) * ratio, view->size() * ratio);
        if (buffer->rect().contains(area)) {
            QPixmap picture = QPixmap::fromImage(buffer->copy(area));
            picture.setDevicePixelRatio(ratio);
            return picture;
        }
    }
    return view->grab();
}

} // namespace

FadeOverlay::FadeOverlay(QWidget *parent) : QWidget(parent)
{
    setAttribute(Qt::WA_TransparentForMouseEvents); // clicks go to the files
    setAttribute(Qt::WA_NoSystemBackground);
    setFocusPolicy(Qt::NoFocus);
    hide();
    m_stats = qEnvironmentVariableIntValue("ARGONAUT_FADE_STATS") != 0;
    m_animation = new QVariantAnimation(this);
    m_animation->setDuration(DurationMs);
    m_animation->setStartValue(1.0);
    m_animation->setEndValue(0.0);
    m_animation->setEasingCurve(QEasingCurve::InOutQuad);
    connect(m_animation, &QVariantAnimation::valueChanged, this, [this](const QVariant &value) {
        m_opacity = value.toReal();
        if (!m_stats) {
            update();
            return;
        }
        QElapsedTimer timer;
        timer.start();
        repaint(); // now, with the view underneath: the whole cost of a frame
        const qint64 us = timer.nsecsElapsed() / 1000;
        m_frameUs += us;
        m_worstUs = qMax(m_worstUs, us);
        ++m_frames;
    });
    connect(m_animation, &QVariantAnimation::finished, this, [this] {
        hide();
        m_picture = QPixmap();
        if (m_stats && m_frames > 0) {
            std::fprintf(stderr, "fade %dx%d: picture %.1f ms, %d frames, %.1f ms each, worst %.1f ms\n",
                         width(), height(), m_grabUs / 1000.0, m_frames, m_frameUs / 1000.0 / m_frames,
                         m_worstUs / 1000.0);
        }
    });
    desktopAnimations(); // ask early: the answer is there before the first fade
}

bool FadeOverlay::enabled()
{
    // The user's own choice wins. Until they make one, do as the desktop
    // says: off where animations are switched off ("reduce animation").
    const QVariant chosen = QSettings().value(QStringLiteral("view/fade"));
    return chosen.isValid() ? chosen.toBool() : desktopAnimations();
}

void FadeOverlay::setEnabled(bool on)
{
    QSettings().setValue(QStringLiteral("view/fade"), on);
}

bool FadeOverlay::isRunning() const
{
    return m_animation->state() == QAbstractAnimation::Running;
}

void FadeOverlay::stop()
{
    if (!isRunning())
        return;
    m_animation->stop();
    hide();
    m_picture = QPixmap();
}

void FadeOverlay::start(QWidget *view)
{
    if (isRunning()) { // folders are changing fast: no effect in the way
        stop();
        return;
    }
    if (!view || !view->isVisible() || view->size().isEmpty() || !enabled())
        return;
    QElapsedTimer timer;
    timer.start();
    m_picture = pictureOf(view);
    m_grabUs = timer.nsecsElapsed() / 1000;
    m_frameUs = m_worstUs = 0;
    m_frames = 0;
    setGeometry(view->geometry());
    m_opacity = 1.0;
    show();
    m_animation->start();
}

void FadeOverlay::paintEvent(QPaintEvent *)
{
    if (m_picture.isNull())
        return;
    QPainter painter(this);
    painter.setOpacity(m_opacity);
    painter.drawPixmap(0, 0, m_picture);
}
