#ifndef ROOMLISTMODEL_HPP_
#define ROOMLISTMODEL_HPP_

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <QVariantList>
#include <QHash>
#include <QList>
#include <QSet>

namespace bb { namespace cascades { class ArrayDataModel; } }
class MatrixApi;
class MediaManager;
class QNetworkReply;
class QTimer;

// Wraps a Cascades ArrayDataModel with the room-list rows BBport shows:
// {roomId, name, avatarMxc, avatarLocalUrl, lastBody, lastSender, lastTs,
//  unreadCount, encrypted, isTyping, isInvite, inviterId}. Sorted by lastTs
// descending, with pending invites always pinned to the top.
//
// m_allRooms is the authoritative full list (every joined/invited room,
// hidden or not); m_model (exposed as "model") is a filtered/sorted
// PROJECTION of it -- rebuildVisible() recomputes m_model from m_allRooms
// whenever a room is added/updated/removed, the search query changes, or a
// room is hidden/unhidden. Hiding is a local-only, per-device preference
// (never synced to the homeserver via account_data): it's just a UI
// convenience for decluttering the inbox, not something other Matrix
// clients need to see, so a plain local file is enough.
class RoomListModel : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QObject* model READ model CONSTANT)
    Q_PROPERTY(QString searchQuery READ searchQuery WRITE setSearchQuery NOTIFY searchQueryChanged)
    // Sum of unreadCount across every room (including hidden ones -- hiding
    // a chat declutters the inbox view, it doesn't mean its messages no
    // longer count as unread). Used by the Active Frame cover (see
    // assets/cover.qml) shown in the multitasking/open-apps screen.
    Q_PROPERTY(int totalUnreadCount READ totalUnreadCount NOTIFY totalUnreadCountChanged)
    // Set by ApplicationUI when the app is invoked by tapping a notification
    // (see NotificationManager, which attaches a bb::system::InvokeRequest
    // carrying the roomId to every Notification it posts). Watched at
    // NavigationPane scope in main.qml (the usual "property mirror" trick,
    // since ApplicationUI has no direct reach into the QML tree) to actually
    // push the conversation page open.
    Q_PROPERTY(QString pendingOpenRoomId READ pendingOpenRoomId NOTIFY pendingOpenRoomIdChanged)

public:
    explicit RoomListModel(MatrixApi *api, MediaManager *media, QObject *parent = 0);
    virtual ~RoomListModel();

    QObject* model() const;
    QString searchQuery() const;
    void setSearchQuery(const QString &query);
    int totalUnreadCount() const;
    QString pendingOpenRoomId() const;

    Q_INVOKABLE int indexOfRoom(const QString &roomId) const;
    Q_INVOKABLE QVariantMap roomAt(int index) const;
    Q_INVOKABLE void acceptInvite(const QString &roomId);
    Q_INVOKABLE void declineInvite(const QString &roomId);

    // Hiding: local-only (see class comment), immediately persisted.
    Q_INVOKABLE void hideRoom(const QString &roomId);
    Q_INVOKABLE void unhideRoom(const QString &roomId);
    Q_INVOKABLE bool isRoomHidden(const QString &roomId) const;
    // Every non-invite room (hidden or not), each item annotated with
    // "hidden": true/false, for a "manage hidden chats" screen.
    Q_INVOKABLE QVariantList allRoomsForManagement() const;

public slots:
    void upsertRoom(const QString &roomId, const QVariantMap &summary);
    void setTyping(const QString &roomId, const QStringList &userIds);
    void addInvite(const QString &roomId, const QString &inviterId, const QString &roomName);
    void removeRoom(const QString &roomId);
    // Called from ApplicationUI's InvokeManager::invoked() handler. Emits
    // pendingOpenRoomIdChanged() unconditionally (not just on a value
    // change) since tapping the same room's notification twice in a row
    // must still re-trigger navigation both times.
    void requestOpenRoom(const QString &roomId);

signals:
    void searchQueryChanged();
    void totalUnreadCountChanged();
    void pendingOpenRoomIdChanged();

private slots:
    void onJoinReplyFinished();
    void onThumbnailReady(const QString &mxcUri, const QString &localFileUrl);
    // A slot so the search-query path can defer it via m_searchDebounce
    // instead of calling it on every keystroke -- see setSearchQuery().
    // Every other caller (upsertRoom, hideRoom, ...) still calls it
    // directly/immediately, unaffected by the debounce.
    void rebuildVisible();

private:
    void loadHiddenRooms();
    void saveHiddenRooms() const;
    void recomputeTotalUnreadCount();

    MatrixApi *m_api;
    MediaManager *m_media;
    bb::cascades::ArrayDataModel *m_model;
    QHash<QString, int> m_indexByRoomId; // roomId -> index within m_model (visible only)
    QList<QVariantMap> m_allRooms;
    QHash<QString, int> m_allIndexByRoomId; // roomId -> index within m_allRooms
    QSet<QString> m_hiddenRoomIds;
    QString m_searchQuery;
    QString m_hiddenRoomsFilePath;
    int m_totalUnreadCount;
    QTimer *m_searchDebounce;
    // Coalesces rebuildVisible() calls from upsertRoom()/addInvite()/
    // removeRoom() -- a single initial /sync response walks every
    // joined room in a tight synchronous loop (SyncEngine::
    // processRoomsObject()), each iteration emitting roomUpdated() straight
    // into upsertRoom(). Without this, a 200-room Beeper account (very
    // real with WhatsApp/iMessage/etc bridges) reran the full O(n) sort +
    // ArrayDataModel clear/rebuild 200 times in a row, entirely on the main
    // thread -- easily seconds of UI-thread blocking, which is both the
    // "fatica a caricare le chat" symptom and, if long enough, something
    // the platform could treat as a hung app. A zero-interval singleShot
    // timer re-armed by every call in the same burst only actually fires
    // once the burst's synchronous call stack unwinds back to the event
    // loop, collapsing N rebuilds into 1. hideRoom()/unhideRoom() skip this
    // and rebuild immediately instead -- those are single explicit taps,
    // not a burst, and immediate feedback reads better there.
    QTimer *m_rebuildDebounce;
    QString m_pendingOpenRoomId;
};

#endif /* ROOMLISTMODEL_HPP_ */
