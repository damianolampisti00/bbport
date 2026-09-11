#include "nativevideoplayer.hpp"

#include <mm/renderer.h>

#include <QTimer>

#include <sys/stat.h>

NativeVideoPlayer::NativeVideoPlayer(QObject *parent) :
        QObject(parent),
        m_connection(0),
        m_context(0),
        m_videoOutputId(-1),
        m_audioOutputId(-1),
        m_positionMs(0),
        m_ticker(new QTimer(this))
{
    m_ticker->setInterval(1000);
    connect(m_ticker, SIGNAL(timeout()), this, SLOT(onTick()));
}

NativeVideoPlayer::~NativeVideoPlayer()
{
    cleanup();
}

int NativeVideoPlayer::positionMs() const
{
    return m_positionMs;
}

void NativeVideoPlayer::onTick()
{
    m_positionMs += 1000;
    emit positionMsChanged();
}

bool NativeVideoPlayer::play(const QString &fileUrl, const QString &windowId, const QString &windowGroup)
{
    cleanup();

    m_connection = mmr_connect(NULL);
    if (!m_connection) return false;

    // Context names must be unique system-wide; qualify with our own
    // pointer value rather than a fixed string in case of rapid reopen.
    QByteArray ctxName = QString("bbportvideo_%1").arg(reinterpret_cast<quintptr>(this)).toUtf8();
    m_context = mmr_context_create(m_connection, ctxName.constData(), 0, S_IRWXU | S_IRWXG | S_IRWXO);
    if (!m_context) {
        cleanup();
        return false;
    }

    // Matches exactly the URL scheme BlackBerry's own NDK-Samples/
    // VideoPlayback uses (confirmed working on-device): mm-renderer creates
    // its own Screen child window here, joining windowGroup under windowId,
    // which a QML ForeignWindowControl with the same windowId/windowGroup
    // binds to.
    QByteArray videoUrl = QString("screen:?wingrp=%1&winid=%2").arg(windowGroup, windowId).toUtf8();
    m_videoOutputId = mmr_output_attach(m_context, videoUrl.constData(), "video");
    if (m_videoOutputId < 0) {
        cleanup();
        return false;
    }

    m_audioOutputId = mmr_output_attach(m_context, "audio:default", "audio");
    if (m_audioOutputId < 0) {
        cleanup();
        return false;
    }

    if (mmr_input_attach(m_context, fileUrl.toUtf8().constData(), "track") != 0) {
        cleanup();
        return false;
    }

    if (mmr_play(m_context) != 0) {
        cleanup();
        return false;
    }

    m_positionMs = 0;
    emit positionMsChanged();
    m_ticker->start();
    return true;
}

void NativeVideoPlayer::pause()
{
    if (m_context) mmr_speed_set(m_context, 0);
    m_ticker->stop();
}

void NativeVideoPlayer::resume()
{
    if (m_context) mmr_speed_set(m_context, 1000);
    if (m_context) m_ticker->start();
}

void NativeVideoPlayer::stop()
{
    cleanup();
}

bool NativeVideoPlayer::seek(int positionMs)
{
    if (!m_context || positionMs < 0) return false;
    // mmr_seek's position argument is a plain decimal string of
    // milliseconds for "track"-type inputs (what play() attaches).
    QByteArray pos = QByteArray::number(positionMs);
    bool ok = mmr_seek(m_context, pos.constData()) == 0;
    if (ok) {
        m_positionMs = positionMs;
        emit positionMsChanged();
    }
    return ok;
}

void NativeVideoPlayer::cleanup()
{
    m_ticker->stop();
    if (m_context) {
        mmr_stop(m_context);
        if (m_audioOutputId >= 0) mmr_output_detach(m_context, m_audioOutputId);
        if (m_videoOutputId >= 0) mmr_output_detach(m_context, m_videoOutputId);
        mmr_context_destroy(m_context);
        m_context = 0;
    }
    if (m_connection) {
        mmr_disconnect(m_connection);
        m_connection = 0;
    }
    m_videoOutputId = -1;
    m_audioOutputId = -1;
    m_positionMs = 0;
    emit positionMsChanged();
}
