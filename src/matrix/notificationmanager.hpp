#ifndef NOTIFICATIONMANAGER_HPP_
#define NOTIFICATIONMANAGER_HPP_

#include <QObject>
#include <QString>
#include <QVariantMap>
#include <QHash>

class MatrixApi;
class SyncEngine;

// Posts a bb::platform::Notification (Hub entry, plus LED/sound/preview per
// whatever the user's system notification settings allow) for an incoming
// message in a room the user isn't currently looking at. Skips our own
// messages (isOutgoing) and reactions/membership changes, which never reach
// timelineEvent() in the first place (SyncEngine only emits it for
// m.room.message/m.sticker). Also skips everything until SyncEngine's
// initial /sync (potentially hundreds of historical events across every
// room) has finished -- without this, first login/launch used to fire a
// notification storm for messages the user already read elsewhere.
//
// Deliberately depends on nothing but MatrixApi and SyncEngine -- both
// already-proven-safe plain QObjects -- rather than RoomListModel/
// MessageListModel directly: ApplicationHeadless's own object graph has no
// UI-facing models at all (RoomListModel specifically owns a
// bb::cascades::ArrayDataModel, not safe to construct outside a real
// Cascades Application), and since neither class inlines its accessors,
// even a guarded/never-actually-called `if (ptr) ptr->method()` would still
// need those symbols at link time, dragging in that whole dependency chain
// regardless of the runtime null check. So instead: setCurrentRoomId()
// gives ApplicationUI a way to report "this room is open right now"
// without exposing MessageListModel itself, and room names for the
// notification title come from listening to SyncEngine's own roomUpdated()
// directly rather than querying RoomListModel for them.
class NotificationManager : public QObject
{
    Q_OBJECT

public:
    explicit NotificationManager(MatrixApi *api, SyncEngine *syncEngine, QObject *parent = 0);

public slots:
    void onTimelineEvent(const QString &roomId, const QVariantMap &event);
    // ApplicationUI forwards MessageListModel::roomIdChanged() here (see its
    // own onCurrentRoomIdChanged() bridge slot) so a notification is
    // suppressed for whichever room is currently open on-screen. Never
    // called at all in ApplicationHeadless, where nothing is ever "open".
    void setCurrentRoomId(const QString &roomId);

private slots:
    void onRoomUpdated(const QString &roomId, const QVariantMap &summary);

private:
    MatrixApi *m_api;
    SyncEngine *m_syncEngine;
    QString m_currentRoomId;
    QHash<QString, QString> m_roomNames; // roomId -> display name, fed by onRoomUpdated()
};

#endif /* NOTIFICATIONMANAGER_HPP_ */
