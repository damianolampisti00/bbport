#ifndef SYNCENGINE_HPP_
#define SYNCENGINE_HPP_

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <QHash>
#include <QSet>
#include <QList>

class MatrixApi;
class KeyBackupManager;
class OlmCryptoManager;
class QNetworkReply;
class QTimer;

// Drives the Matrix /sync long-poll loop and fans out parsed events as Qt
// signals. Does not own any UI-facing model: RoomListModel and
// MessageListModel subscribe to these signals independently.
class SyncEngine : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool running READ isRunning NOTIFY runningChanged)
    // False from login until the very first /sync response (the "initial
    // sync" -- potentially hundreds of historical events across every room)
    // has been fully processed. main.qml shows a loading spinner gated on
    // this instead of the room list, and NotificationManager uses it to
    // never post a notification for anything that arrived as part of that
    // initial catch-up -- see initialSyncCompleted()'s doc comment below.
    Q_PROPERTY(bool initialSyncDone READ isInitialSyncDone NOTIFY initialSyncCompleted)

public:
    explicit SyncEngine(MatrixApi *api, KeyBackupManager *keyBackup, OlmCryptoManager *olmCrypto, QObject *parent = 0);
    virtual ~SyncEngine();

    bool isRunning() const;
    bool isInitialSyncDone() const;

public slots:
    void start();
    void stop();
    // For a short-running headless invocation (see ApplicationHeadless),
    // not the continuously-polling foreground app: does exactly one /sync
    // round trip (success, a normal idle long-poll timeout with nothing
    // new, or a genuine failure alike -- see singleSyncFinished()), then
    // stops without touching the persisted since-token/room cache the way
    // stop() deliberately does for logout. Whatever ran (timelineEvent(),
    // etc.) has already fired by the time singleSyncFinished() does.
    void startOnce();

signals:
    void runningChanged();
    // Fires exactly once, only after startOnce() (never after start()),
    // once that single /sync round trip has fully concluded one way or
    // another. ok is true for a real response OR a normal "nothing new"
    // long-poll timeout, false only for a genuine transport/parse failure
    // -- either way there's always a next scheduled wakeup to try again,
    // so this never retries on its own the way the continuous loop does.
    void singleSyncFinished(bool ok);
    // Fires exactly once per app session, right after the first /sync
    // response finishes processing (see the initialSyncDone Q_PROPERTY
    // above). Note this fires AFTER every timelineEvent()/roomUpdated() etc.
    // for that same initial batch has already gone out -- a listener that
    // needs to distinguish "this event is part of the initial catch-up"
    // from "this is genuinely new" must check isInitialSyncDone() itself at
    // the time each event arrives, not just react to this signal.
    void initialSyncCompleted();
    // summary keys: name, avatarMxc, lastBody, lastSender, lastTs, unreadCount
    void roomUpdated(const QString &roomId, const QVariantMap &summary);
    // event keys: eventId, sender, msgtype, body, ts, mediaMxc, isOutgoing
    void timelineEvent(const QString &roomId, const QVariantMap &event);
    // Same event keys as timelineEvent(), matched to an existing timeline
    // entry by eventId; used when a Megolm session arrives after we already
    // had to show the "encrypted" placeholder for that event.
    void timelineEventUpdated(const QString &roomId, const QVariantMap &event);
    void typingUpdated(const QString &roomId, const QStringList &userIds);
    void receiptUpdated(const QString &roomId, const QString &eventId, const QStringList &userIds);
    // An m.reaction (m.annotation relation) targeting eventId, plaintext or
    // decrypted from an E2EE room alike.
    void reactionAdded(const QString &roomId, const QString &targetEventId, const QString &key, const QString &sender, const QString &reactionEventId);
    // A live m.room.redaction event deleting eventId. The other case --
    // fetching a message that was ALREADY redacted before we ever saw it
    // (unsigned.redacted_because present on the event itself) -- doesn't
    // need this signal at all: it's handled inline by building the "deleted"
    // placeholder directly into the very first timelineEvent()/
    // timelineEventUpdated() emitted for that event, since there's no prior
    // cached copy anywhere to patch.
    void eventRedacted(const QString &roomId, const QString &eventId);
    void inviteReceived(const QString &roomId, const QString &inviterId, const QString &roomName);
    void syncError(const QString &message);
    // Fired once per room the first time we see it, with the /sync
    // "prev_batch" token marking the point just before its earliest synced
    // message -- the starting point for paginating older history via
    // GET /rooms/{roomId}/messages?dir=b&from=<token>.
    void roomHistoryAnchor(const QString &roomId, const QString &prevBatch);
    // One raw to-device event from /sync's "to_device.events" (keys:
    // type, sender, content), for OlmCryptoManager to process.
    void toDeviceEvent(const QVariantMap &event);

private slots:
    void onSyncReplyFinished();
    void onWatchdogTimeout();
    void onKeySessionReady(const QString &roomId, const QString &sessionId);
    // Retries every room/session pair currently waiting on a Megolm session
    // (see m_pendingEncrypted) once KeyBackupManager::unlock() succeeds --
    // those requestSession() calls all no-op'd earlier while still locked.
    void onKeyBackupUnlocked();
    // A slot (not just a private method) so a failed sync can be retried via
    // QTimer::singleShot(backoffMs, this, SLOT(doSync())) instead of calling
    // it immediately in a tight loop -- see onSyncReplyFinished().
    void doSync();

private:
    struct PendingEncryptedEvent {
        QString eventId;
        QString sender;
        qint64 ts;
        QString ciphertext;
    };

    // Persists the whole m_roomMeta hash to disk (see roomCacheFilePath()'s
    // comment); called after every successful sync that changed anything.
    void saveRoomCache();
    // Loads a previously-saved m_roomMeta and, for each room found, emits
    // roomUpdated() immediately so the room list can populate before the
    // first network response of this launch even lands. Called from
    // start(), not the constructor, since ApplicationUI only wires
    // roomUpdated() through to RoomListModel after constructing SyncEngine.
    void loadRoomCache();
    void processRoomsObject(const QVariantMap &roomsObj);
    void processJoinedRoom(const QString &roomId, const QVariantMap &roomObj);
    void processInvitedRoom(const QString &roomId, const QVariantMap &roomObj);
    QString displayNameFor(const QString &roomId, const QVariantMap &memberEvent);
    void recordMemberInfo(const QString &roomId, const QVariantMap &memberEvent);
    // The best name known for this sender in this room: their m.room.member
    // displayname if we've seen one, else the MXID local part.
    QString senderNameFor(const QString &roomId, const QString &sender) const;
    bool emitDecryptedMessage(const QString &roomId, const QString &sender, const QString &eventId, qint64 ts, const QString &plaintextJson, bool isUpdate, QVariantMap *outBuiltEvent = 0);

    MatrixApi *m_api;
    KeyBackupManager *m_keyBackup;
    OlmCryptoManager *m_olmCrypto;
    bool m_running;
    bool m_singleShot; // set by startOnce(), never by start() -- see its own doc comment
    bool m_initialSyncDone;
    QString m_since;
    QNetworkReply *m_currentReply;
    QTimer *m_watchdog;
    // Current retry delay after a genuine /sync failure (bad JSON, network
    // error) -- 0 means "no failure streak, retry immediately next time",
    // otherwise doubles per consecutive failure up to a cap. Reset to 0 on
    // any successful sync. Keeps a flaky/lost connection from retrying in a
    // tight loop that would otherwise hammer the radio and CPU continuously.
    int m_retryBackoffMs;

    // General runtime instrumentation (see conversation): timing/counts for
    // the current sync cycle, logged via qDebug in onSyncReplyFinished() so
    // a real usage session can be reviewed afterward for anomalies (a cycle
    // taking far longer than expected, a spike in decrypt failures, ...)
    // without needing to reproduce anything live.
    qint64 m_cycleStartMs;
    int m_cycleDecryptOk;
    int m_cycleDecryptFailed;

    // Per-room cached metadata used to build the summaries emitted via
    // roomUpdated (explicit name wins over heuristic 1:1 naming).
    QHash<QString, QVariantMap> m_roomMeta;

    // roomId -> userId -> avatar_url (mxc://) / display name, from whatever
    // m.room.member events have been seen so far (best-effort:
    // lazy_load_members means we only ever see members who've actually
    // posted or are otherwise referenced, same limitation displayNameFor()
    // already lives with).
    QHash<QString, QHash<QString, QString> > m_memberAvatars;
    QHash<QString, QHash<QString, QString> > m_memberDisplayNames;
    // roomId -> distinct non-self userIds seen via m.room.member so far --
    // a lower-bound estimate of room size (same lazy-loading caveat as
    // above) used to decide whether to show a sender name/avatar on
    // messages at all: pointless clutter in a 1:1 DM, needed in a group.
    QHash<QString, QSet<QString> > m_roomMemberIds;

    // Encrypted events waiting on a Megolm session we don't have yet, keyed
    // by "roomId|sessionId" so onKeySessionReady() can patch them in once
    // KeyBackupManager fetches the session from the server backup.
    QHash<QString, QList<PendingEncryptedEvent> > m_pendingEncrypted;
};

#endif /* SYNCENGINE_HPP_ */
