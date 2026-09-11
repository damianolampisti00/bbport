#ifndef TIMELINESTORE_HPP_
#define TIMELINESTORE_HPP_

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <QVariantList>
#include <QHash>
#include <QSet>

// Keeps a bounded per-room timeline cache (events, typing users, read
// receipts) fed directly from SyncEngine signals, independent of whichever
// room's MessageListModel currently happens to be on screen. This is what
// lets you background a conversation, read three others, and come back to
// find the first one still has everything that arrived meanwhile.
class TimelineStore : public QObject
{
    Q_OBJECT

public:
    explicit TimelineStore(QObject *parent = 0);
    virtual ~TimelineStore();

    QVariantList eventsForRoom(const QString &roomId) const;
    QStringList typingUsersForRoom(const QString &roomId) const;

    // Pagination token for GET /rooms/{roomId}/messages?dir=b to fetch the
    // next (older) page of history for this room. Empty once exhausted.
    QString prevBatchFor(const QString &roomId) const;
    // Always overwrites -- called after each successful history page fetch
    // to advance the token further into the past.
    void setPrevBatch(const QString &roomId, const QString &token);
    // Prepends a block of older events (already oldest-first) fetched via
    // history pagination, deduping against what's already cached, and
    // announces just the newly-added ones via historyPrepended().
    void prependHistory(const QString &roomId, const QVariantList &events);

signals:
    void eventAppended(const QString &roomId, const QVariantMap &event);
    void eventUpdated(const QString &roomId, const QVariantMap &event);
    void historyPrepended(const QString &roomId, const QVariantList &events);
    void typingChanged(const QString &roomId, const QStringList &userIds);
    void receiptChanged(const QString &roomId, const QString &eventId, const QStringList &userIds);
    // reactions: [{key, count}, ...], one entry per distinct emoji currently
    // annotating this event.
    void reactionsChanged(const QString &roomId, const QString &eventId, const QVariantList &reactions);

public slots:
    void onTimelineEvent(const QString &roomId, const QVariantMap &event);
    void onTimelineEventUpdated(const QString &roomId, const QVariantMap &event);
    void onTypingUpdated(const QString &roomId, const QStringList &userIds);
    void onReceiptUpdated(const QString &roomId, const QString &eventId, const QStringList &userIds);
    // An m.reaction (m.annotation) targeting targetEventId. Aggregated
    // per-key with one count per distinct sender (a user reacting twice with
    // the same key is not double-counted); redactions (un-reacting) aren't
    // handled anywhere in this app yet, so a reaction can't currently be
    // removed once counted.
    void onReactionAdded(const QString &roomId, const QString &targetEventId, const QString &key, const QString &sender, const QString &reactionEventId);
    // A live m.room.redaction targeting eventId (see SyncEngine, which
    // detects both this and the "already redacted before we ever saw it"
    // case -- unsigned.redacted_because on the original event -- but only
    // emits this signal for the live case, since the latter is baked
    // directly into the event this store first caches). Replaces the
    // cached event's content with a "deleted" placeholder in place,
    // preserving sender/ts/etc., and re-announces it via the same
    // eventUpdated() every other in-place content change uses.
    void onEventRedacted(const QString &roomId, const QString &eventId);
    // First-write-wins: set from SyncEngine's roomHistoryAnchor() the first
    // time a room is seen, ignored afterwards so a later incremental sync's
    // prev_batch can't clobber the true pagination starting point.
    void onRoomHistoryAnchor(const QString &roomId, const QString &prevBatch);

private:
    static const int kMaxEventsPerRoom = 300;

    QHash<QString, QList<QVariantMap> > m_events;
    QHash<QString, QStringList> m_typingUsers;
    QHash<QString, QString> m_prevBatch;
    QHash<QString, QHash<QString, QSet<QString> > > m_reactions; // "roomId|eventId" -> key -> senders
};

#endif /* TIMELINESTORE_HPP_ */
