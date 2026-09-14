#ifndef NATIVEVIDEOPLAYER_HPP_
#define NATIVEVIDEOPLAYER_HPP_

#include <QObject>
#include <QString>

class QTimer;

typedef struct mmr_connection mmr_connection_t;
typedef struct mmr_context mmr_context_t;

// Drives video playback via the raw mm-renderer client API (mm/renderer.h)
// instead of bb::multimedia::MediaPlayer. Confirmed by direct A/B test on a
// real device: a bare mm-renderer + Screen sample (BlackBerry's own
// NDK-Samples/VideoPlayback, no Cascades involved at all) shows video fine
// on this exact Q5, while bb::multimedia::MediaPlayer -- going through
// ForeignWindowControl the documented way, with binding/windowAttached/
// videoDimensions all confirmed firing correctly -- renders solid black
// regardless of codec, resolution, or any QML-level configuration tried.
// The underlying platform mechanism works; something in Cascades' own
// MediaPlayer wrapper does not. This class talks to mm-renderer the same
// way that working sample does (an mmr_output_attach with a
// "screen:?wingrp=...&winid=..." URL matching a QML ForeignWindowControl,
// which already proved it can bind to a window created this way), bypassing
// whatever MediaPlayer does differently.
class NativeVideoPlayer : public QObject
{
    Q_OBJECT
    // mm-renderer's real position/duration are only available via its PPS
    // event stream, which needs the same BPS event-loop integration this
    // whole class exists to avoid (see the class comment on why raw
    // mm-renderer was used at all instead of bb::multimedia::MediaPlayer).
    // So this is a locally-ticked approximation: a 1-second QTimer while
    // playing, reset by seek()/play(). Good enough for a scrub bar; not
    // exact if the stream stalls/buffers.
    Q_PROPERTY(int positionMs READ positionMs NOTIFY positionMsChanged)

public:
    explicit NativeVideoPlayer(QObject *parent = 0);
    virtual ~NativeVideoPlayer();

    int positionMs() const;

    // windowId/windowGroup must match a ForeignWindowControl in the QML
    // scene (windowGroup is typically ForeignWindowControl.windowGroup,
    // already defaulting to the main Cascades window group). fileUrl is a
    // "file://..." path exactly like MediaManager already hands out.
    // destWidth/destHeight (pixels, matching that same ForeignWindowControl's
    // own preferredWidth/preferredHeight) set mm-renderer's video_dest_w/h --
    // BlackBerry's own reference VideoPlayback sample always sets this via
    // mmr_output_parameters() before anything is visible; omitting it left
    // mm-renderer with no defined destination rectangle to render into,
    // which is plausibly why video stayed black even once every other part
    // of this class matched that sample.
    Q_INVOKABLE bool play(const QString &fileUrl, const QString &windowId, const QString &windowGroup, int destWidth, int destHeight);
    Q_INVOKABLE void pause();
    Q_INVOKABLE void resume();
    Q_INVOKABLE void stop();
    // Jumps to an absolute position (milliseconds from the start of the
    // track). No-op (returns false) if nothing is currently attached.
    Q_INVOKABLE bool seek(int positionMs);

signals:
    void positionMsChanged();

private slots:
    void onTick();

private:
    void cleanup();

    mmr_connection_t *m_connection;
    mmr_context_t *m_context;
    int m_videoOutputId;
    int m_audioOutputId;
    int m_positionMs;
    QTimer *m_ticker;
};

#endif /* NATIVEVIDEOPLAYER_HPP_ */
