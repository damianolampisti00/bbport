#include "timelinestore.hpp"

#include <QSet>
#include <QHashIterator>

TimelineStore::TimelineStore(QObject *parent) :
        QObject(parent)
{
}

TimelineStore::~TimelineStore()
{
}

QVariantList TimelineStore::eventsForRoom(const QString &roomId) const
{
    QVariantList result;
    QList<QVariantMap> events = m_events.value(roomId);
    for (int i = 0; i < events.size(); ++i) result << events.at(i);
    return result;
}

QStringList TimelineStore::typingUsersForRoom(const QString &roomId) const
{
    return m_typingUsers.value(roomId);
}

void TimelineStore::onTimelineEvent(const QString &roomId, const QVariantMap &event)
{
    QList<QVariantMap> &events = m_events[roomId];

    QString eventId = event.value("eventId").toString();
    if (!eventId.isEmpty()) {
        for (int i = 0; i < events.size(); ++i) {
            if (events.at(i).value("eventId").toString() == eventId) return; // dedupe
        }
    }

    events.append(event);
    while (events.size() > kMaxEventsPerRoom) events.removeFirst();

    emit eventAppended(roomId, event);
}

void TimelineStore::onTimelineEventUpdated(const QString &roomId, const QVariantMap &event)
{
    QList<QVariantMap> &events = m_events[roomId];
    QString eventId = event.value("eventId").toString();
    for (int i = 0; i < events.size(); ++i) {
        if (events.at(i).value("eventId").toString() == eventId) {
            events[i] = event;
            emit eventUpdated(roomId, event);
            return;
        }
    }
}

void TimelineStore::onEventRedacted(const QString &roomId, const QString &eventId)
{
    QList<QVariantMap> &events = m_events[roomId];
    for (int i = 0; i < events.size(); ++i) {
        if (events.at(i).value("eventId").toString() != eventId) continue;

        QVariantMap redacted = events.at(i);
        redacted["msgtype"] = "m.text";
        redacted["body"] = QString::fromUtf8("Message deleted");
        redacted["isRedacted"] = true;
        // Redaction strips the original content server-side per spec --
        // clear anything that would otherwise still try to render/download
        // stale media, a reply preview, or an Instagram Reel link for a
        // message that no longer has any of that.
        redacted.remove("mediaMxc");
        redacted.remove("mediaLocalUrl");
        redacted.remove("mediaKey");
        redacted.remove("mediaIv");
        redacted.remove("mediaHash");
        redacted.remove("mediaWidth");
        redacted.remove("mediaHeight");
        redacted.remove("mediaDuration");
        redacted.remove("replyToEventId");
        redacted.remove("replyPreview");
        redacted.remove("instagramUrl");

        events[i] = redacted;
        emit eventUpdated(roomId, redacted);
        return;
    }
}

QString TimelineStore::prevBatchFor(const QString &roomId) const
{
    return m_prevBatch.value(roomId);
}

void TimelineStore::setPrevBatch(const QString &roomId, const QString &token)
{
    m_prevBatch[roomId] = token;
}

void TimelineStore::onRoomHistoryAnchor(const QString &roomId, const QString &prevBatch)
{
    if (m_prevBatch.contains(roomId)) return;
    m_prevBatch[roomId] = prevBatch;
}

void TimelineStore::prependHistory(const QString &roomId, const QVariantList &events)
{
    QList<QVariantMap> &existing = m_events[roomId];

    QSet<QString> existingIds;
    for (int i = 0; i < existing.size(); ++i) {
        QString id = existing.at(i).value("eventId").toString();
        if (!id.isEmpty()) existingIds.insert(id);
    }

    QList<QVariantMap> toPrepend;
    QVariantList announced;
    for (int i = 0; i < events.size(); ++i) {
        QVariantMap ev = events.at(i).toMap();
        QString id = ev.value("eventId").toString();
        if (!id.isEmpty() && existingIds.contains(id)) continue;
        toPrepend.append(ev);
        announced << ev;
    }
    if (toPrepend.isEmpty()) return;

    existing = toPrepend + existing;
    emit historyPrepended(roomId, announced);
}

void TimelineStore::onTypingUpdated(const QString &roomId, const QStringList &userIds)
{
    m_typingUsers[roomId] = userIds;
    emit typingChanged(roomId, userIds);
}

void TimelineStore::onReactionAdded(const QString &roomId, const QString &targetEventId, const QString &key, const QString &sender, const QString &reactionEventId)
{
    Q_UNUSED(reactionEventId);
    QString mapKey = roomId + "|" + targetEventId;
    QSet<QString> &senders = m_reactions[mapKey][key];
    if (senders.contains(sender)) return; // already counted
    senders.insert(sender);

    QVariantList reactions;
    QHash<QString, QSet<QString> > perKey = m_reactions.value(mapKey);
    QHashIterator<QString, QSet<QString> > it(perKey);
    while (it.hasNext()) {
        it.next();
        if (it.value().isEmpty()) continue;
        QVariantMap r;
        r["key"] = it.key();
        r["count"] = it.value().size();
        reactions << r;
    }

    QList<QVariantMap> &events = m_events[roomId];
    for (int i = 0; i < events.size(); ++i) {
        if (events.at(i).value("eventId").toString() == targetEventId) {
            QVariantMap ev = events.at(i);
            ev["reactions"] = reactions;
            events[i] = ev;
            break;
        }
    }
    emit reactionsChanged(roomId, targetEventId, reactions);
}

void TimelineStore::onReceiptUpdated(const QString &roomId, const QString &eventId, const QStringList &userIds)
{
    QList<QVariantMap> &events = m_events[roomId];
    for (int i = 0; i < events.size(); ++i) {
        if (events.at(i).value("eventId").toString() == eventId) {
            QVariantMap ev = events.at(i);
            ev["readBy"] = userIds;
            events[i] = ev;
            break;
        }
    }
    emit receiptChanged(roomId, eventId, userIds);
}
