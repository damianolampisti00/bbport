#ifndef NOTIFICATIONMANAGER_HPP_
#define NOTIFICATIONMANAGER_HPP_

#include <QObject>
#include <QString>
#include <QVariantMap>

class MatrixApi;
class MessageListModel;
class RoomListModel;
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
class NotificationManager : public QObject
{
    Q_OBJECT

public:
    explicit NotificationManager(MatrixApi *api, MessageListModel *messageListModel, RoomListModel *roomListModel, SyncEngine *syncEngine, QObject *parent = 0);

public slots:
    void onTimelineEvent(const QString &roomId, const QVariantMap &event);

private:
    MatrixApi *m_api;
    MessageListModel *m_messageListModel;
    RoomListModel *m_roomListModel;
    SyncEngine *m_syncEngine;
};

#endif /* NOTIFICATIONMANAGER_HPP_ */
