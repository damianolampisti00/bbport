#include "notificationmanager.hpp"
#include "matrixapi.hpp"
#include "syncengine.hpp"

#include <bb/platform/Notification>
#include <bb/platform/NotificationDefaultApplicationSettings>
#include <bb/platform/NotificationPriorityPolicy>
#include <bb/platform/NotificationSettingsError>
#include <bb/system/InvokeRequest>

// Must match the <invoke-target id="..."> declared in bar-descriptor.xml.
// A first attempt used the app's own <id> (it.bbport.client) here on the
// assumption that a self-targeted invoke needs no separate registration --
// that was wrong: with no matching invoke-target, the Invocation Framework
// has nothing to resolve setTarget()'s id to, so tapping the notification
// silently did nothing instead of foregrounding the app on that room.
static const char *const kAppInvokeTarget = "it.bbport.client.notification";

NotificationManager::NotificationManager(MatrixApi *api, SyncEngine *syncEngine, QObject *parent) :
        QObject(parent),
        m_api(api),
        m_syncEngine(syncEngine)
{
    if (m_syncEngine) {
        connect(m_syncEngine, SIGNAL(roomUpdated(QString,QVariantMap)), this, SLOT(onRoomUpdated(QString,QVariantMap)));
    }

    // Instant Preview (the top-of-screen popup banner, like the system Hub
    // apps show) is NotApplicable -- silently disabled, with the per-app
    // toggle hidden from Settings entirely -- for any app by default unless
    // it has a BlackBerry Hub account. There's no Hub account integration
    // here, but NotificationDefaultApplicationSettings::setPreview() can
    // still explicitly opt this app's notifications into Allow. Per the
    // apply() docs this only takes effect the *first* time it's ever called
    // for the app (a no-op, returning None, on every later call once it's
    // taken effect or the user has since changed it themselves in
    // Settings), so it's safe to call unconditionally on every startup.
    bb::platform::NotificationDefaultApplicationSettings settings;
    settings.setPreview(bb::platform::NotificationPriorityPolicy::Allow);
    settings.apply();
}

void NotificationManager::setCurrentRoomId(const QString &roomId)
{
    m_currentRoomId = roomId;
}

void NotificationManager::onRoomUpdated(const QString &roomId, const QVariantMap &summary)
{
    QString name = summary.value("name").toString();
    if (!name.isEmpty()) m_roomNames[roomId] = name;
}

void NotificationManager::onTimelineEvent(const QString &roomId, const QVariantMap &event)
{
    if (m_syncEngine && !m_syncEngine->isInitialSyncDone()) return;
    if (event.value("isOutgoing").toBool()) return;
    if (!m_currentRoomId.isEmpty() && roomId == m_currentRoomId) return; // already viewing this room

    QString msgtype = event.value("msgtype").toString();
    QString body = event.value("body").toString();
    QString preview;
    if (msgtype == "m.image") preview = QString::fromUtf8("\xf0\x9f\x93\xb7 Photo");
    else if (msgtype == "m.video") preview = QString::fromUtf8("\xf0\x9f\x8e\xac Video");
    else if (msgtype == "m.audio") preview = QString::fromUtf8("\xf0\x9f\x8e\xb5 Voice message");
    else if (msgtype == "m.file") preview = QString::fromUtf8("\xf0\x9f\x93\x84 File");
    else if (msgtype == "m.sticker") preview = QString::fromUtf8("Sticker");
    else preview = body;
    if (preview.isEmpty()) return;

    QString senderName = event.value("senderName").toString();
    if (senderName.isEmpty()) senderName = event.value("sender").toString();

    QString roomName = m_roomNames.value(roomId);

    QString title = (!roomName.isEmpty() && roomName != senderName)
            ? QString("%1 (%2)").arg(senderName, roomName)
            : senderName;

    // Tapping the notification re-invokes this same app (see
    // ApplicationUI's InvokeManager::invoked() handler) carrying roomId as
    // the payload, so it can jump straight to that conversation instead of
    // just opening to whatever screen was last showing.
    bb::system::InvokeRequest invokeRequest;
    invokeRequest.setTarget(kAppInvokeTarget);
    invokeRequest.setAction("bb.action.OPEN");
    invokeRequest.setMimeType("application/x-bbport-room");
    invokeRequest.setData(roomId.toUtf8());

    bb::platform::Notification *notification = new bb::platform::Notification(this);
    notification->setTitle(title);
    notification->setBody(preview);
    notification->setInvokeRequest(invokeRequest);
    notification->notify();
    notification->deleteLater();
}
