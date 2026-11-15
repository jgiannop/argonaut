#pragma once

#include <QPixmap>
#include <QWidget>

class QVariantAnimation;

// The short cross-fade when a pane changes folder. start() takes one picture
// of the view as it is, lays it over the view and fades it out; the new
// folder loads underneath at full speed, so nothing waits for the effect.
//
// A change of folder while a fade is running ends it at once and starts no
// new one: going through folders quickly is never slowed down. On or off is
// the user's choice (Preferences, "view/fade"); until they choose, it does
// as the desktop says, so it is off where animations are switched off.
class FadeOverlay : public QWidget
{
    Q_OBJECT

public:
    explicit FadeOverlay(QWidget *parent); // the pane's view stack

    // Call before the view changes. `view` is the widget to take the picture of.
    void start(QWidget *view);
    void stop();
    bool isRunning() const;

    static bool enabled();            // the preference, else the desktop's setting
    static void setEnabled(bool on);  // the preference
    static constexpr int DurationMs = 140;

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    QPixmap m_picture;
    qreal m_opacity = 0;
    QVariantAnimation *m_animation;
    // ARGONAUT_FADE_STATS=1: frames are painted at once and timed.
    bool m_stats = false;
    qint64 m_grabUs = 0, m_frameUs = 0, m_worstUs = 0;
    int m_frames = 0;
};
