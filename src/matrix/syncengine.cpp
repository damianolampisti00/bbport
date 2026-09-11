#include "syncengine.hpp"
#include "matrixapi.hpp"
#include "keybackupmanager.hpp"
#include "olmcryptomanager.hpp"

#include <bb/data/JsonDataAccess>

#include <QNetworkReply>
#include <QTimer>
#include <QFile>
#include <QTextStream>
#include <QDir>
#include <QMapIterator>
#include <QHashIterator>

using namespace bb::data;

static const int kSyncTimeoutMs = 30000;
static const int kFirstSyncTimeoutMs = 10000;
static const int kWatchdogGraceMs = 20000;
static const int kRetryBackoffInitialMs = 1000;
static const int kRetryBackoffMaxMs = 30000;

// Private to this app (QDir::homePath(), not shared/misc) since nothing
// outside BBport itself ever needs to read it -- just the next_batch token
// from the last successful /sync, so a relaunch (or a crash recovery) can
// resume with an incremental sync instead of paying for a full initial one
// again (which re-fetches every room's recent history and, before
// initialSyncDone existed, used to fire a notification for all of it).
static QString sinceTokenFilePath()
{
    return QDir::homePath() + "/sync_since_token.txt";
}

// The whole m_roomMeta hash (per-room name/avatar/last-message/etc, plus the
// hasExplicitName/hasExplicitAvatar bookkeeping flags -- NOT just the
// "summary" subset roomUpdated() emits) so a resumed incremental sync has
// something to show immediately instead of blank/raw-roomId rows: an
// incremental /sync only re-sends STATE THAT CHANGED, so a room with no new
// m.room.name/m.room.avatar since last launch would otherwise never refill
// those fields at all in a from-scratch process. Keeping the bookkeeping
// flags too (not just the display fields) also avoids a heuristic 1:1-name/
// avatar guess incorrectly overwriting an already-known explicit one the
// moment any member event shows up in the resumed sync's delta.
static QString roomCacheFilePath()
{
    return QDir::homePath() + "/room_cache.json";
}

// Media messages carry their mxc:// URI as plain content.url in unencrypted
// rooms, but as content.file.url (alongside the AES-256-CTR key/iv/hash
// needed to decrypt it, per the Matrix "EncryptedFile" format) in E2EE
// rooms -- MediaManager::resolve() is a no-op-for-decryption when
// key/iv/hash are left empty, so this always sets mediaMxc the same way
// regardless of which shape was present.
static void extractMediaFields(const QVariantMap &content, QVariantMap *outEvent)
{
    QString url = content.value("url").toString();
    if (!url.isEmpty()) {
        (*outEvent)["mediaMxc"] = url;
    } else {
        QVariantMap file = content.value("file").toMap();
        QString fileUrl = file.value("url").toString();
        if (!fileUrl.isEmpty()) {
            (*outEvent)["mediaMxc"] = fileUrl;
            (*outEvent)["mediaKey"] = file.value("key").toMap().value("k").toString();
            (*outEvent)["mediaIv"] = file.value("iv").toString();
            (*outEvent)["mediaHash"] = file.value("hashes").toMap().value("sha256").toString();
        }
    }
    QVariantMap info = content.value("info").toMap();
    (*outEvent)["mediaWidth"] = info.value("w").toInt();
    (*outEvent)["mediaHeight"] = info.value("h").toInt();
    (*outEvent)["mediaDuration"] = info.value("duration").toInt(); // ms, m.audio/m.video only

    // Beeper's Instagram bridge sends a Reel/post as a plain m.image (just
    // the static thumbnail) plus this external_url pointing at the real
    // Instagram page -- never an actual playable video. Flagging it here
    // lets the UI offer "watch as video", which fetches and extracts the
    // real video on-device via yt-dlp (see MediaManager::fetchInstagramVideo())
    // -- this is HTML/page scraping under the hood, so it can break whenever
    // Instagram changes their page markup.
    QString externalUrl = content.value("external_url").toString();
    if (externalUrl.contains("instagram.com/p/") || externalUrl.contains("instagram.com/reel/")) {
        (*outEvent)["instagramUrl"] = externalUrl;
    }
}

// Rich replies (m.relates_to.m.in_reply_to.event_id) carry a legacy
// plain-text fallback baked into body -- one or more lines prefixed "> "
// quoting the parent, then a blank separator line, then the reply's own
// text -- so older clients show something reasonable. Returns the parent
// event id (empty if this isn't a reply) and, when it is, rewrites *body in
// place to just the replier's own text with the fallback stripped, per the
// spec's "stripping the fallback" algorithm.
static QString stripReplyFallback(const QVariantMap &content, QString *body)
{
    QString targetEventId = content.value("m.relates_to").toMap().value("m.in_reply_to").toMap().value("event_id").toString();
    if (targetEventId.isEmpty()) return QString();

    QStringList lines = body->split('\n');
    int i = 0;
    while (i < lines.size() && lines.at(i).startsWith("> ")) ++i;
    if (i < lines.size() && lines.at(i).isEmpty()) ++i; // blank separator line
    if (i > 0) *body = QStringList(lines.mid(i)).join("\n");
    return targetEventId;
}

// True for an m.reaction event with an m.annotation relation, filling the
// target event id and emoji key. False (out-params untouched) for anything
// else, including malformed reactions with no usable relation.
static bool parseReaction(const QString &type, const QVariantMap &content, QString *targetEventId, QString *key)
{
    if (type != "m.reaction") return false;
    QVariantMap relatesTo = content.value("m.relates_to").toMap();
    if (relatesTo.value("rel_type").toString() != "m.annotation") return false;
    *targetEventId = relatesTo.value("event_id").toString();
    *key = relatesTo.value("key").toString();
    return !targetEventId->isEmpty() && !key->isEmpty();
}

SyncEngine::SyncEngine(MatrixApi *api, KeyBackupManager *keyBackup, OlmCryptoManager *olmCrypto, QObject *parent) :
        QObject(parent),
        m_api(api),
        m_keyBackup(keyBackup),
        m_olmCrypto(olmCrypto),
        m_running(false),
        m_initialSyncDone(false),
        m_currentReply(0),
        m_watchdog(new QTimer(this)),
        m_retryBackoffMs(0)
{
    m_watchdog->setSingleShot(true);
    connect(m_watchdog, SIGNAL(timeout()), this, SLOT(onWatchdogTimeout()));
    if (m_keyBackup) {
        connect(m_keyBackup, SIGNAL(sessionReady(QString,QString)), this, SLOT(onKeySessionReady(QString,QString)));
        connect(m_keyBackup, SIGNAL(unlockedChanged()), this, SLOT(onKeyBackupUnlocked()));
    }

    // Resume from a previous session's last-known sync position, if any --
    // see sinceTokenFilePath()'s comment. A resumed sync is never the giant
    // historical dump an actual from-scratch initial sync is (bounded by
    // real elapsed time, not by "everything since account creation"), so
    // there's no need to gate the spinner/notifications on it either.
    QFile tokenFile(sinceTokenFilePath());
    if (tokenFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        m_since = QString::fromUtf8(tokenFile.readAll()).trimmed();
        tokenFile.close();
        if (!m_since.isEmpty()) m_initialSyncDone = true;
    }
}

SyncEngine::~SyncEngine()
{
}

bool SyncEngine::isRunning() const
{
    return m_running;
}

bool SyncEngine::isInitialSyncDone() const
{
    return m_initialSyncDone;
}

void SyncEngine::start()
{
    if (m_running) return;
    m_running = true;
    emit runningChanged();
    loadRoomCache();
    doSync();
}

void SyncEngine::stop()
{
    if (!m_running) return;
    m_running = false;
    m_watchdog->stop();
    if (m_currentReply) {
        m_currentReply->disconnect(this);
        m_currentReply->abort();
        m_currentReply = 0;
    }
    // stop() is only ever called from the logout button (see main.qml) --
    // never for a transient pause -- so the persisted sync position is
    // this account's now, not the next login's (possibly a different
    // account entirely). A resumed sync with a foreign/expired token isn't
    // dangerous (the homeserver just answers with a fresh one and its own
    // full sync), but it would defeat the whole point of skipping that.
    QFile::remove(sinceTokenFilePath());
    QFile::remove(roomCacheFilePath());
    m_since.clear();
    m_initialSyncDone = false;
    m_roomMeta.clear();
    emit runningChanged();
}

void SyncEngine::saveRoomCache()
{
    QVariantMap allRooms;
    QHashIterator<QString, QVariantMap> it(m_roomMeta);
    while (it.hasNext()) {
        it.next();
        allRooms[it.key()] = it.value();
    }

    JsonDataAccess jda;
    QByteArray buffer;
    jda.saveToBuffer(QVariant(allRooms), &buffer);
    QFile file(roomCacheFilePath());
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        file.write(buffer);
    }
}

void SyncEngine::loadRoomCache()
{
    QFile file(roomCacheFilePath());
    if (!file.open(QIODevice::ReadOnly)) return;
    QByteArray buffer = file.readAll();
    file.close();

    JsonDataAccess jda;
    QVariant parsed = jda.loadFromBuffer(buffer);
    if (jda.hasError()) return;

    QVariantMap cached = parsed.toMap();
    QMapIterator<QString, QVariant> it(cached);
    while (it.hasNext()) {
        it.next();
        QString roomId = it.key();
        QVariantMap meta = it.value().toMap();
        m_roomMeta[roomId] = meta;

        QVariantMap summary;
        summary["name"] = meta.value("name", roomId).toString();
        summary["avatarMxc"] = meta.value("avatarMxc").toString();
        summary["lastBody"] = meta.value("lastBody").toString();
        summary["lastSender"] = meta.value("lastSender").toString();
        summary["lastTs"] = meta.value("lastTs").toLongLong();
        summary["unreadCount"] = meta.value("unreadCount").toInt();
        summary["encrypted"] = meta.value("encrypted").toBool();
        emit roomUpdated(roomId, summary);
    }
}

void SyncEngine::doSync()
{
    if (!m_running) return;

    QVariantMap query;
    if (m_since.isEmpty()) {
        query["timeout"] = QString::number(kFirstSyncTimeoutMs);
        query["filter"] = QString(
            "{\"room\":{\"timeline\":{\"limit\":25},"
            "\"state\":{\"lazy_load_members\":true}},"
            "\"presence\":{\"types\":[]}}");
    } else {
        query["since"] = m_since;
        query["timeout"] = QString::number(kSyncTimeoutMs);
    }

    m_currentReply = m_api->apiGet("/sync", query);
    connect(m_currentReply, SIGNAL(finished()), this, SLOT(onSyncReplyFinished()));
    m_watchdog->start((m_since.isEmpty() ? kFirstSyncTimeoutMs : kSyncTimeoutMs) + kWatchdogGraceMs);
}

void SyncEngine::onWatchdogTimeout()
{
    if (m_currentReply) {
        m_currentReply->abort();
    }
}

void SyncEngine::onSyncReplyFinished()
{
    m_watchdog->stop();
    QNetworkReply *reply = qobject_cast<QNetworkReply*>(sender());
    if (reply != m_currentReply) {
        if (reply) reply->deleteLater();
        return;
    }
    m_currentReply = 0;

    bool ok = false;
    QVariant parsed = MatrixApi::parseJson(reply, &ok);
    QNetworkReply::NetworkError netError = reply->error();
    reply->deleteLater();

    if (!m_running) return;

    if (!ok) {
        if (netError == QNetworkReply::OperationCanceledError) {
            // Our own watchdog aborting a long-poll that ran past its
            // timeout -- a normal part of the /sync cycle, not a failure,
            // so retry immediately and don't touch the backoff streak.
            doSync();
            return;
        }
        emit syncError("Sync failed: invalid response from server.");
        // A genuine failure (network error, bad JSON, server error): retry
        // with exponential backoff instead of immediately, so a lost/flaky
        // connection can't turn into a tight loop hammering the radio and
        // CPU with requests that just fail again instantly.
        m_retryBackoffMs = (m_retryBackoffMs == 0) ? kRetryBackoffInitialMs : qMin(m_retryBackoffMs * 2, kRetryBackoffMaxMs);
        QTimer::singleShot(m_retryBackoffMs, this, SLOT(doSync()));
        return;
    }

    m_retryBackoffMs = 0;

    QVariantMap root = parsed.toMap();
    m_since = root.value("next_batch").toString();
    if (!m_since.isEmpty()) {
        QFile tokenFile(sinceTokenFilePath());
        if (tokenFile.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
            tokenFile.write(m_since.toUtf8());
        }
    } else {
        // A resumed sync whose saved token the homeserver no longer
        // recognizes (expired/GC'd) comes back as a parseable-but-tokenless
        // error body -- ok stays true (see MatrixApi::parseJson), but
        // there's no next_batch to carry forward. Falls through to a full
        // initial sync on the very next doSync() (m_since is empty again),
        // so undo the "already resumed" latch too: that next sync really is
        // the giant historical dump initialSyncDone/notification-gating
        // exists for, not a routine incremental poll.
        m_initialSyncDone = false;
        QFile::remove(sinceTokenFilePath());
    }

    QVariantList toDeviceEvents = root.value("to_device").toMap().value("events").toList();
    for (int i = 0; i < toDeviceEvents.size(); ++i) {
        emit toDeviceEvent(toDeviceEvents.at(i).toMap());
    }

    QVariantMap roomsObj = root.value("rooms").toMap();
    processRoomsObject(roomsObj);
    // A routine idle long-poll (the common case: nothing happened for
    // ~30s) comes back with an empty "rooms" object -- saveRoomCache() used
    // to run unconditionally here regardless, meaning a full JSON
    // serialize + disk write of every room's metadata happened roughly
    // every 30 seconds for the entire time the app sits idle, forever.
    // Skipping it when nothing could have changed matches what the
    // function's own doc comment already claimed ("called after every
    // successful sync that changed anything") but the code didn't actually
    // implement.
    if (!roomsObj.isEmpty()) saveRoomCache();

    if (!m_initialSyncDone) {
        m_initialSyncDone = true;
        emit initialSyncCompleted();
    }

    doSync();
}

void SyncEngine::processRoomsObject(const QVariantMap &roomsObj)
{
    QVariantMap joinObj = roomsObj.value("join").toMap();
    QMapIterator<QString, QVariant> joinIt(joinObj);
    while (joinIt.hasNext()) {
        joinIt.next();
        processJoinedRoom(joinIt.key(), joinIt.value().toMap());
    }

    QVariantMap inviteObj = roomsObj.value("invite").toMap();
    QMapIterator<QString, QVariant> inviteIt(inviteObj);
    while (inviteIt.hasNext()) {
        inviteIt.next();
        processInvitedRoom(inviteIt.key(), inviteIt.value().toMap());
    }
}

QString SyncEngine::displayNameFor(const QString &roomId, const QVariantMap &memberEvent)
{
    Q_UNUSED(roomId);
    QVariantMap content = memberEvent.value("content").toMap();
    QString displayName = content.value("displayname").toString();
    if (!displayName.isEmpty()) return displayName;
    QString mxid = memberEvent.value("state_key").toString();
    int colonIdx = mxid.indexOf(':');
    QString local = colonIdx > 0 ? mxid.mid(1, colonIdx - 1) : mxid;
    return local.isEmpty() ? mxid : local;
}

QString SyncEngine::senderNameFor(const QString &roomId, const QString &sender) const
{
    QString name = m_memberDisplayNames.value(roomId).value(sender);
    if (!name.isEmpty()) return name;
    int colonIdx = sender.indexOf(':');
    QString local = colonIdx > 0 ? sender.mid(1, colonIdx - 1) : sender;
    return local.isEmpty() ? sender : local;
}

void SyncEngine::recordMemberInfo(const QString &roomId, const QVariantMap &memberEvent)
{
    QString mxid = memberEvent.value("state_key").toString();
    if (mxid.isEmpty()) return;
    QVariantMap content = memberEvent.value("content").toMap();

    QString avatarUrl = content.value("avatar_url").toString();
    if (!avatarUrl.isEmpty()) m_memberAvatars[roomId][mxid] = avatarUrl;

    QString displayName = content.value("displayname").toString();
    if (!displayName.isEmpty()) m_memberDisplayNames[roomId][mxid] = displayName;

    if (mxid != m_api->userId()) m_roomMemberIds[roomId].insert(mxid);
}

void SyncEngine::processJoinedRoom(const QString &roomId, const QVariantMap &roomObj)
{
    QVariantMap meta = m_roomMeta.value(roomId);
    QString selfUserId = m_api->userId();

    // --- state events: room name, avatar, member display names ---
    QVariantList stateEvents = roomObj.value("state").toMap().value("events").toList();
    for (int i = 0; i < stateEvents.size(); ++i) {
        QVariantMap ev = stateEvents.at(i).toMap();
        QString type = ev.value("type").toString();
        if (type == "m.room.name") {
            QString name = ev.value("content").toMap().value("name").toString();
            if (!name.isEmpty()) {
                meta["name"] = name;
                meta["hasExplicitName"] = true;
            }
        } else if (type == "m.room.avatar") {
            QString url = ev.value("content").toMap().value("url").toString();
            if (!url.isEmpty()) {
                meta["avatarMxc"] = url;
                meta["hasExplicitAvatar"] = true;
            }
        } else if (type == "m.room.member") {
            QString mxid = ev.value("state_key").toString();
            if (mxid != selfUserId) {
                if (!meta.value("hasExplicitName").toBool()) {
                    meta["name"] = displayNameFor(roomId, ev);
                }
                // DMs never set m.room.avatar (per spec/convention) --
                // clients are expected to show the other member's own
                // profile picture instead, same as the name fallback above.
                // A "group" room (more than one other member) can't
                // sensibly borrow a single member's avatar this way, but by
                // the time a second distinct other member shows up here,
                // hasExplicitAvatar being unset just means this keeps
                // getting overwritten with whichever member was seen last --
                // acceptable since real groups almost always DO set their
                // own m.room.avatar in practice.
                if (!meta.value("hasExplicitAvatar").toBool()) {
                    QString avatarUrl = ev.value("content").toMap().value("avatar_url").toString();
                    if (!avatarUrl.isEmpty()) meta["avatarMxc"] = avatarUrl;
                }
            }
            recordMemberInfo(roomId, ev);
        } else if (type == "m.room.encryption") {
            meta["encrypted"] = true;
        }
    }

    // --- timeline events: messages (and inline member changes for naming) ---
    QVariantMap timeline = roomObj.value("timeline").toMap();

    if (!meta.value("historyAnchorSent").toBool()) {
        QString prevBatch = timeline.value("prev_batch").toString();
        if (!prevBatch.isEmpty()) {
            meta["historyAnchorSent"] = true;
            emit roomHistoryAnchor(roomId, prevBatch);
        }
    }

    QVariantList timelineEvents = timeline.value("events").toList();
    for (int i = 0; i < timelineEvents.size(); ++i) {
        QVariantMap ev = timelineEvents.at(i).toMap();
        QString type = ev.value("type").toString();

        if (type == "m.room.member") {
            QString mxid = ev.value("state_key").toString();
            if (mxid != selfUserId) {
                if (!meta.value("hasExplicitName").toBool()) {
                    meta["name"] = displayNameFor(roomId, ev);
                }
                if (!meta.value("hasExplicitAvatar").toBool()) {
                    QString avatarUrl = ev.value("content").toMap().value("avatar_url").toString();
                    if (!avatarUrl.isEmpty()) meta["avatarMxc"] = avatarUrl;
                }
            }
            recordMemberInfo(roomId, ev);
            continue;
        }
        if (type == "m.room.name") {
            QString name = ev.value("content").toMap().value("name").toString();
            if (!name.isEmpty()) {
                meta["name"] = name;
                meta["hasExplicitName"] = true;
            }
            continue;
        }
        if (type == "m.room.avatar") {
            QString url = ev.value("content").toMap().value("url").toString();
            if (!url.isEmpty()) {
                meta["avatarMxc"] = url;
                meta["hasExplicitAvatar"] = true;
            }
            continue;
        }
        if (type == "m.room.redaction") {
            // A live deletion of a message we may already have cached/
            // displayed (see TimelineStore::onEventRedacted(), which does
            // the actual lookup-and-patch since only it holds the cache).
            // "redacts" moved from content to the top level of the event in
            // newer room versions; older ones still put it in content, so
            // check both.
            QString redactedEventId = ev.value("redacts").toString();
            if (redactedEventId.isEmpty()) redactedEventId = ev.value("content").toMap().value("redacts").toString();
            if (!redactedEventId.isEmpty()) emit eventRedacted(roomId, redactedEventId);
            continue;
        }
        // A message that was ALREADY redacted before we ever saw it (e.g.
        // deleted before this device joined/synced, or paginated in from
        // history) carries redacted_because instead of ever needing a
        // separate m.room.redaction to arrive -- content is also stripped
        // to {} server-side by this point, so there's nothing to decrypt/
        // parse even for what was originally an m.room.encrypted event.
        // Building the placeholder here, before the type-specific (and for
        // encrypted rooms, decryption-attempting) branches below, avoids
        // e.g. a redacted encrypted message wrongly showing "cannot
        // decrypt" instead of "deleted".
        if (ev.value("unsigned").toMap().contains("redacted_because")) {
            QVariantMap outEvent;
            outEvent["eventId"] = ev.value("event_id").toString();
            outEvent["sender"] = ev.value("sender").toString();
            outEvent["msgtype"] = "m.text";
            outEvent["body"] = QString::fromUtf8("Message deleted");
            outEvent["ts"] = ev.value("origin_server_ts").toLongLong();
            outEvent["isOutgoing"] = (ev.value("sender").toString() == selfUserId);
            outEvent["isRedacted"] = true;
            emit timelineEvent(roomId, outEvent);
            meta["lastBody"] = QString::fromUtf8("Message deleted");
            meta["lastSender"] = ev.value("sender").toString();
            meta["lastTs"] = outEvent.value("ts").toLongLong();
            continue;
        }
        if (type == "m.room.encrypted") {
            QVariantMap content = ev.value("content").toMap();
            QString sender = ev.value("sender").toString();
            QString eventId = ev.value("event_id").toString();
            qint64 ts = ev.value("origin_server_ts").toLongLong();
            QString algorithm = content.value("algorithm").toString();
            QString sessionId = content.value("session_id").toString();
            QString ciphertext = content.value("ciphertext").toString();
            QString senderKey = content.value("sender_key").toString();

            meta["encrypted"] = true;

            QString plaintextJson;
            bool sessionDecrypted = m_keyBackup && algorithm == "m.megolm.v1.aes-sha2"
                    && m_keyBackup->decrypt(roomId, sessionId, ciphertext, &plaintextJson);

            bool decrypted = sessionDecrypted;
            if (sessionDecrypted) {
                // handled: true whenever the inner event was successfully
                // decoded, even if it wasn't message-shaped (e.g. a
                // reaction -- emitDecryptedMessage() emits reactionAdded()
                // itself and leaves *builtEvent empty in that case, so this
                // correctly skips the meta/lastBody update below without
                // falling through to the "couldn't decrypt" placeholder,
                // which used to happen for every reaction/sticker sent into
                // an E2EE room).
                QVariantMap builtEvent;
                bool handled = emitDecryptedMessage(roomId, sender, eventId, ts, plaintextJson, false, &builtEvent);
                if (!handled) {
                    decrypted = false;
                } else if (!builtEvent.isEmpty()) {
                    QString msgtype = builtEvent.value("msgtype").toString();
                    QString body = builtEvent.value("body").toString();
                    meta["lastBody"] = msgtype == "m.text" ? body : (msgtype == "m.image" ? QString::fromUtf8("\xf0\x9f\x93\xb7 Photo") : body);
                    meta["lastSender"] = sender;
                    meta["lastTs"] = ts;
                }
            }

            if (!decrypted) {
                QVariantMap outEvent;
                outEvent["eventId"] = eventId;
                outEvent["sender"] = sender;
                outEvent["msgtype"] = "m.text";
                outEvent["body"] = QString::fromUtf8("\xf0\x9f\x94\x92 Encrypted message (not supported)");
                outEvent["ts"] = ts;
                outEvent["isOutgoing"] = (sender == selfUserId);
                emit timelineEvent(roomId, outEvent);

                meta["lastBody"] = QString::fromUtf8("\xf0\x9f\x94\x92 Encrypted message");
                meta["lastSender"] = sender;
                meta["lastTs"] = ts;

                if (m_keyBackup && algorithm == "m.megolm.v1.aes-sha2" && !sessionId.isEmpty()) {
                    QString pendingKey = roomId + "|" + sessionId;
                    // Only ask once per session, the first time we hit a
                    // ciphertext we can't decrypt for it -- otherwise every
                    // later message in the same still-missing session would
                    // fire its own m.room_key_request, spamming this
                    // account's other devices for something already asked.
                    bool alreadyPending = m_pendingEncrypted.contains(pendingKey);
                    PendingEncryptedEvent pending;
                    pending.eventId = eventId;
                    pending.sender = sender;
                    pending.ts = ts;
                    pending.ciphertext = ciphertext;
                    m_pendingEncrypted[pendingKey].append(pending);
                    m_keyBackup->requestSession(roomId, sessionId);
                    // Recovers the case where the m.room_key that should
                    // have shared this session either never reached us (a
                    // stale/desynced 1:1 Olm session with the sender
                    // silently drops it -- see olmDecryptFrom()) or was sent
                    // before this device existed: asks any of this
                    // account's OWN other devices that already has the
                    // session to forward it via m.forwarded_room_key,
                    // independently of the server-side key-backup fetch
                    // requestSession() above already does.
                    if (!alreadyPending && m_olmCrypto) {
                        m_olmCrypto->requestRoomKey(roomId, sessionId, senderKey);
                    }
                }
            }
            continue;
        }
        if (type == "m.reaction") {
            QVariantMap content = ev.value("content").toMap();
            QString targetEventId, key;
            if (parseReaction(type, content, &targetEventId, &key)) {
                emit reactionAdded(roomId, targetEventId, key, ev.value("sender").toString(), ev.value("event_id").toString());
            }
            continue;
        }
        if (type != "m.room.message" && type != "m.sticker") continue;

        QVariantMap content = ev.value("content").toMap();
        QString sender = ev.value("sender").toString();
        QString msgtype = type == "m.sticker" ? QString("m.sticker") : content.value("msgtype").toString();
        QString body = content.value("body").toString();
        QString replyToEventId = stripReplyFallback(content, &body);
        qint64 ts = ev.value("origin_server_ts").toLongLong();

        QVariantMap outEvent;
        outEvent["eventId"] = ev.value("event_id").toString();
        outEvent["sender"] = sender;
        outEvent["msgtype"] = msgtype;
        outEvent["body"] = body;
        outEvent["ts"] = ts;
        outEvent["isOutgoing"] = (sender == selfUserId);
        outEvent["senderAvatarMxc"] = m_memberAvatars.value(roomId).value(sender);
        outEvent["senderName"] = senderNameFor(roomId, sender);
        outEvent["isGroupChat"] = m_roomMemberIds.value(roomId).size() > 1;
        if (!replyToEventId.isEmpty()) outEvent["replyToEventId"] = replyToEventId;
        if (msgtype == "m.image" || msgtype == "m.file" || msgtype == "m.video" || msgtype == "m.audio" || msgtype == "m.sticker") {
            extractMediaFields(content, &outEvent);
        }
        emit timelineEvent(roomId, outEvent);

        meta["lastBody"] = msgtype == "m.text" ? body : (msgtype == "m.image" ? QString::fromUtf8("\xf0\x9f\x93\xb7 Photo") : body);
        meta["lastSender"] = sender;
        meta["lastTs"] = ts;
    }

    // --- ephemeral events: typing + read receipts ---
    QVariantList ephemeralEvents = roomObj.value("ephemeral").toMap().value("events").toList();
    for (int i = 0; i < ephemeralEvents.size(); ++i) {
        QVariantMap ev = ephemeralEvents.at(i).toMap();
        QString type = ev.value("type").toString();
        QVariantMap content = ev.value("content").toMap();

        if (type == "m.typing") {
            QStringList userIds;
            QVariantList ids = content.value("user_ids").toList();
            for (int j = 0; j < ids.size(); ++j) {
                QString uid = ids.at(j).toString();
                if (uid != selfUserId) userIds << uid;
            }
            emit typingUpdated(roomId, userIds);
        } else if (type == "m.receipt") {
            QMapIterator<QString, QVariant> evIt(content);
            while (evIt.hasNext()) {
                evIt.next();
                QString eventId = evIt.key();
                QVariantMap readers = evIt.value().toMap().value("m.read").toMap();
                QStringList userIds;
                QMapIterator<QString, QVariant> readerIt(readers);
                while (readerIt.hasNext()) {
                    readerIt.next();
                    if (readerIt.key() != selfUserId) userIds << readerIt.key();
                }
                if (!userIds.isEmpty()) emit receiptUpdated(roomId, eventId, userIds);
            }
        }
    }

    // --- unread counters ---
    QVariantMap unread = roomObj.value("unread_notifications").toMap();
    meta["unreadCount"] = unread.value("notification_count").toInt();

    m_roomMeta[roomId] = meta;

    QVariantMap summary;
    summary["name"] = meta.value("name", roomId).toString();
    summary["avatarMxc"] = meta.value("avatarMxc").toString();
    summary["lastBody"] = meta.value("lastBody").toString();
    summary["lastSender"] = meta.value("lastSender").toString();
    summary["lastTs"] = meta.value("lastTs").toLongLong();
    summary["unreadCount"] = meta.value("unreadCount").toInt();
    summary["encrypted"] = meta.value("encrypted").toBool();
    emit roomUpdated(roomId, summary);
}

void SyncEngine::processInvitedRoom(const QString &roomId, const QVariantMap &roomObj)
{
    QVariantList inviteStateEvents = roomObj.value("invite_state").toMap().value("events").toList();
    QString roomName = roomId;
    QString inviterId;
    for (int i = 0; i < inviteStateEvents.size(); ++i) {
        QVariantMap ev = inviteStateEvents.at(i).toMap();
        QString type = ev.value("type").toString();
        if (type == "m.room.name") {
            QString name = ev.value("content").toMap().value("name").toString();
            if (!name.isEmpty()) roomName = name;
        } else if (type == "m.room.member" && ev.value("content").toMap().value("membership").toString() == "invite") {
            inviterId = ev.value("sender").toString();
        }
    }
    emit inviteReceived(roomId, inviterId, roomName);
}

bool SyncEngine::emitDecryptedMessage(const QString &roomId, const QString &sender, const QString &eventId, qint64 ts, const QString &plaintextJson, bool isUpdate, QVariantMap *outBuiltEvent)
{
    JsonDataAccess jda;
    QVariant parsed = jda.loadFromBuffer(plaintextJson.toUtf8());
    if (jda.hasError()) return false;

    QVariantMap innerEvent = parsed.toMap();
    QString innerType = innerEvent.value("type").toString();
    QVariantMap content = innerEvent.value("content").toMap();

    if (innerType == "m.reaction") {
        // Handled here, not as a displayable event: *outBuiltEvent stays
        // empty on purpose, which the caller (processJoinedRoom's
        // m.room.encrypted branch) treats as "decrypted fine, nothing more
        // to show" rather than falling back to the "couldn't decrypt"
        // placeholder.
        QString targetEventId, key;
        if (parseReaction(innerType, content, &targetEventId, &key)) {
            emit reactionAdded(roomId, targetEventId, key, sender, eventId);
        }
        return true;
    }

    if (innerType != "m.room.message" && innerType != "m.sticker") return false;

    QString msgtype = innerType == "m.sticker" ? QString("m.sticker") : content.value("msgtype").toString();
    QString body = content.value("body").toString();
    QString replyToEventId = stripReplyFallback(content, &body);

    QVariantMap outEvent;
    outEvent["eventId"] = eventId;
    outEvent["sender"] = sender;
    outEvent["msgtype"] = msgtype;
    outEvent["body"] = body;
    outEvent["ts"] = ts;
    outEvent["isOutgoing"] = (sender == m_api->userId());
    outEvent["senderAvatarMxc"] = m_memberAvatars.value(roomId).value(sender);
    outEvent["senderName"] = senderNameFor(roomId, sender);
    outEvent["isGroupChat"] = m_roomMemberIds.value(roomId).size() > 1;
    if (!replyToEventId.isEmpty()) outEvent["replyToEventId"] = replyToEventId;
    if (msgtype == "m.image" || msgtype == "m.file" || msgtype == "m.video" || msgtype == "m.audio" || msgtype == "m.sticker") {
        extractMediaFields(content, &outEvent);
    }

    if (isUpdate) emit timelineEventUpdated(roomId, outEvent);
    else emit timelineEvent(roomId, outEvent);

    if (outBuiltEvent) *outBuiltEvent = outEvent;
    return true;
}

void SyncEngine::onKeyBackupUnlocked()
{
    // Messages that failed to decrypt (and got their "encrypted" placeholder
    // shown) *before* the backup was unlocked already called requestSession()
    // once, but it no-op'd immediately since m_keyBackup wasn't unlocked yet
    // -- so no HTTP request was ever actually made for them, and nothing
    // retries them on its own. Re-issue those now that we can actually fetch
    // sessions from the server backup.
    if (!m_keyBackup || !m_keyBackup->isUnlocked()) return;
    QStringList pendingKeys = m_pendingEncrypted.keys();
    for (int i = 0; i < pendingKeys.size(); ++i) {
        int sep = pendingKeys.at(i).indexOf('|');
        if (sep < 0) continue;
        QString roomId = pendingKeys.at(i).left(sep);
        QString sessionId = pendingKeys.at(i).mid(sep + 1);
        m_keyBackup->requestSession(roomId, sessionId);
    }
}

void SyncEngine::onKeySessionReady(const QString &roomId, const QString &sessionId)
{
    QList<PendingEncryptedEvent> pending = m_pendingEncrypted.take(roomId + "|" + sessionId);
    if (pending.isEmpty() || !m_keyBackup) return;

    QVariantMap meta = m_roomMeta.value(roomId);
    bool metaChanged = false;

    for (int i = 0; i < pending.size(); ++i) {
        const PendingEncryptedEvent &ev = pending.at(i);
        QString plaintextJson;
        if (!m_keyBackup->decrypt(roomId, sessionId, ev.ciphertext, &plaintextJson)) continue;

        QVariantMap builtEvent;
        if (!emitDecryptedMessage(roomId, ev.sender, ev.eventId, ev.ts, plaintextJson, true, &builtEvent)) continue;
        if (builtEvent.isEmpty()) continue; // handled (e.g. a reaction), not a displayable event

        if (ev.ts >= meta.value("lastTs").toLongLong()) {
            QString msgtype = builtEvent.value("msgtype").toString();
            QString body = builtEvent.value("body").toString();
            meta["lastBody"] = msgtype == "m.text" ? body : (msgtype == "m.image" ? QString::fromUtf8("\xf0\x9f\x93\xb7 Photo") : body);
            meta["lastSender"] = ev.sender;
            meta["lastTs"] = ev.ts;
            metaChanged = true;
        }
    }

    if (!metaChanged) return;
    m_roomMeta[roomId] = meta;
    saveRoomCache();

    QVariantMap summary;
    summary["name"] = meta.value("name", roomId).toString();
    summary["avatarMxc"] = meta.value("avatarMxc").toString();
    summary["lastBody"] = meta.value("lastBody").toString();
    summary["lastSender"] = meta.value("lastSender").toString();
    summary["lastTs"] = meta.value("lastTs").toLongLong();
    summary["unreadCount"] = meta.value("unreadCount").toInt();
    summary["encrypted"] = meta.value("encrypted").toBool();
    emit roomUpdated(roomId, summary);
}
