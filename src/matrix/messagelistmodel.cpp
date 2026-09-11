#include "messagelistmodel.hpp"
#include "matrixapi.hpp"
#include "timelinestore.hpp"
#include "mediamanager.hpp"
#include "keybackupmanager.hpp"
#include "olmcryptomanager.hpp"

#include <bb/cascades/ArrayDataModel>
#include <bb/data/JsonDataAccess>
#include <bb/system/Clipboard>

#include <QNetworkReply>
#include <QTimer>
#include <QUrl>
#include <QDateTime>
#include <QFileInfo>
#include <QMapIterator>

using namespace bb::cascades;
using namespace bb::data;

static const int kTypingStopDelayMs = 4000;
static const int kHistoryPageSize = 30;

MessageRowActions::MessageRowActions(MessageListModel *owner) :
        QObject(owner), m_owner(owner)
{
}

void MessageRowActions::setData(const QString &eventId, const QString &senderId, const QString &senderShort, const QString &bodyPreview, const QString &msgtype, bool isOutgoing)
{
    m_eventId = eventId;
    m_senderId = senderId;
    m_senderShort = senderShort;
    m_bodyPreview = bodyPreview;
    m_msgtype = msgtype;
    m_isOutgoing = isOutgoing;
}

void MessageRowActions::reply()
{
    if (m_owner) m_owner->setReplyTarget(m_eventId, m_senderId, m_senderShort, m_bodyPreview);
}

void MessageRowActions::like()
{
    if (m_owner) m_owner->sendReaction(m_eventId, QString::fromUtf8("\xe2\x9d\xa4\xef\xb8\x8f")); // ❤️
}

void MessageRowActions::edit()
{
    if (m_owner && m_msgtype == "m.text" && m_isOutgoing) m_owner->setEditTarget(m_eventId, m_bodyPreview);
}

void MessageRowActions::copy()
{
    if (m_msgtype != "m.text") return;
    bb::system::Clipboard clipboard;
    clipboard.clear();
    clipboard.insert("text/plain", m_bodyPreview.toUtf8());
}

void MessageRowActions::remove()
{
    if (m_owner) m_owner->redactMessage(m_eventId);
}

// Same msgtype->preview-label mapping the composer's reply banner needs,
// previously duplicated in QML (main.qml's onTriggered) -- centralized here
// since rowActionsFor() below needs it too now.
static QString previewTextFor(const QVariantMap &item)
{
    QString msgtype = item.value("msgtype").toString();
    if (msgtype == "m.text") return item.value("body").toString();
    if (msgtype == "m.image") return QString::fromUtf8("Photo");
    if (msgtype == "m.video") return QString::fromUtf8("Video");
    if (msgtype == "m.audio") return QString::fromUtf8("Voice message");
    if (msgtype == "m.sticker") return QString::fromUtf8("Sticker");
    return QString::fromUtf8("File");
}

// Same shape as SyncEngine's helper of the same name: media messages carry
// their mxc:// URI as plain content.url in unencrypted rooms, or as
// content.file.url (plus the AES-256-CTR key/iv/hash) in E2EE rooms.
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
    (*outEvent)["mediaDuration"] = info.value("duration").toInt();

    // Same Instagram-Reel detection as SyncEngine's helper of the same name.
    QString externalUrl = content.value("external_url").toString();
    if (externalUrl.contains("instagram.com/p/") || externalUrl.contains("instagram.com/reel/")) {
        (*outEvent)["instagramUrl"] = externalUrl;
    }
}

// Same shape as SyncEngine's helper of the same name.
static QString stripReplyFallback(const QVariantMap &content, QString *body)
{
    QString targetEventId = content.value("m.relates_to").toMap().value("m.in_reply_to").toMap().value("event_id").toString();
    if (targetEventId.isEmpty()) return QString();

    QStringList lines = body->split('\n');
    int i = 0;
    while (i < lines.size() && lines.at(i).startsWith("> ")) ++i;
    if (i < lines.size() && lines.at(i).isEmpty()) ++i;
    if (i > 0) *body = QStringList(lines.mid(i)).join("\n");
    return targetEventId;
}

// Same shape as SyncEngine's helper of the same name.
static bool parseReaction(const QString &type, const QVariantMap &content, QString *targetEventId, QString *key)
{
    if (type != "m.reaction") return false;
    QVariantMap relatesTo = content.value("m.relates_to").toMap();
    if (relatesTo.value("rel_type").toString() != "m.annotation") return false;
    *targetEventId = relatesTo.value("event_id").toString();
    *key = relatesTo.value("key").toString();
    return !targetEventId->isEmpty() && !key->isEmpty();
}

MessageListModel::MessageListModel(MatrixApi *api, TimelineStore *store, MediaManager *media, KeyBackupManager *keyBackup, OlmCryptoManager *olmCrypto, QObject *parent) :
        QObject(parent),
        m_api(api),
        m_store(store),
        m_media(media),
        m_keyBackup(keyBackup),
        m_olmCrypto(olmCrypto),
        m_model(new ArrayDataModel(this)),
        m_sending(false),
        m_loadingHistory(false),
        m_historyExhausted(false),
        m_typingActiveSent(false),
        m_typingStopTimer(new QTimer(this)),
        m_roomMembersKnown(false),
        m_roomIsGroupChat(false),
        m_audioIsPlaying(false),
        m_audioPositionMs(0),
        m_audioDurationMs(0),
        m_seekRequestMs(0)
{
    m_instagramVideoResult["ok"] = false;
    m_instagramVideoResult["eventId"] = QString();
    connect(m_store, SIGNAL(eventAppended(QString,QVariantMap)), this, SLOT(onEventAppended(QString,QVariantMap)));
    connect(m_store, SIGNAL(eventUpdated(QString,QVariantMap)), this, SLOT(onEventUpdated(QString,QVariantMap)));
    connect(m_store, SIGNAL(historyPrepended(QString,QVariantList)), this, SLOT(onHistoryPrepended(QString,QVariantList)));
    connect(m_store, SIGNAL(typingChanged(QString,QStringList)), this, SLOT(onTypingChanged(QString,QStringList)));
    connect(m_store, SIGNAL(receiptChanged(QString,QString,QStringList)), this, SLOT(onReceiptChanged(QString,QString,QStringList)));
    connect(m_store, SIGNAL(reactionsChanged(QString,QString,QVariantList)), this, SLOT(onReactionsChanged(QString,QString,QVariantList)));
    connect(m_media, SIGNAL(mediaReady(QString,QString)), this, SLOT(onMediaReady(QString,QString)));
    connect(m_media, SIGNAL(mediaFailed(QString)), this, SLOT(onMediaFailed(QString)));
    connect(m_media, SIGNAL(thumbnailReady(QString,QString)), this, SLOT(onThumbnailReady(QString,QString)));
    connect(m_media, SIGNAL(instagramVideoReady(QString,QString)), this, SLOT(onInstagramVideoReady(QString,QString)));
    connect(m_media, SIGNAL(instagramVideoFailed(QString)), this, SLOT(onInstagramVideoFailed(QString)));
    connect(m_media, SIGNAL(uploadFinished(QString,QString,QString,bool)), this, SLOT(onUploadFinished(QString,QString,QString,bool)));
    connect(m_olmCrypto, SIGNAL(sendSucceeded(QString)), this, SLOT(onEncryptedSendSucceeded(QString)));
    connect(m_olmCrypto, SIGNAL(sendFailed(QString,QString)), this, SLOT(onEncryptedSendFailed(QString,QString)));

    m_typingStopTimer->setSingleShot(true);
    connect(m_typingStopTimer, SIGNAL(timeout()), this, SLOT(onTypingStopTimeout()));
}

MessageListModel::~MessageListModel()
{
}

QString MessageListModel::roomId() const
{
    return m_roomId;
}

QObject* MessageListModel::model() const
{
    return m_model;
}

QString MessageListModel::typingText() const
{
    return m_typingText;
}

bool MessageListModel::isSending() const
{
    return m_sending;
}

QString MessageListModel::lastSendError() const
{
    return m_lastSendError;
}

void MessageListModel::setSending(bool sending)
{
    if (m_sending == sending) return;
    m_sending = sending;
    emit sendingChanged();
}

bool MessageListModel::isLoadingHistory() const
{
    return m_loadingHistory;
}

bool MessageListModel::isHistoryExhausted() const
{
    return m_historyExhausted;
}

void MessageListModel::setLoadingHistory(bool loading)
{
    if (m_loadingHistory == loading) return;
    m_loadingHistory = loading;
    emit loadingHistoryChanged();
}

void MessageListModel::setHistoryExhausted(bool exhausted)
{
    if (m_historyExhausted == exhausted) return;
    m_historyExhausted = exhausted;
    emit historyExhaustedChanged();
}

// A ListItemComponent delegate in this Cascades build turns out NOT to
// inherit main.qml's document context at all -- confirmed on-device via the
// QML debug console, which showed "ReferenceError: Can't find variable:
// messageListModel" for every binding inside the message bubble delegate
// that referenced this object directly, even though the exact same
// identifier resolves fine at Page/ListView scope (e.g. in
// messageView.onTriggered). ListItemData -- the per-row QVariantMap --
// is the only channel that reliably reaches the delegate, so playback state
// is written into the relevant row's own item instead of being exposed only
// as a Q_PROPERTY for the delegate to (unsuccessfully) reach out for.
void MessageListModel::patchAudioPlaybackItem(const QString &eventId, bool playing, int positionMs, int durationMs)
{
    if (eventId.isEmpty()) return;
    for (int i = 0; i < m_model->size(); ++i) {
        QVariantMap item = m_model->value(i).toMap();
        if (item.value("eventId").toString() == eventId) {
            item["isPlayingAudio"] = playing;
            item["audioLivePositionMs"] = positionMs;
            item["audioLiveDurationMs"] = durationMs;
            m_model->replace(i, item);
            break;
        }
    }
}

QString MessageListModel::playingAudioEventId() const { return m_playingAudioEventId; }

void MessageListModel::setPlayingAudioEventId(const QString &eventId)
{
    if (m_playingAudioEventId == eventId) return;
    // Clear the previous track's row so it doesn't keep showing stale
    // position/duration or a "playing" chip once a different one is loaded.
    if (!m_playingAudioEventId.isEmpty()) {
        patchAudioPlaybackItem(m_playingAudioEventId, false, 0, 0);
    }
    m_playingAudioEventId = eventId;
    emit playingAudioEventIdChanged();
    patchAudioPlaybackItem(m_playingAudioEventId, m_audioIsPlaying, m_audioPositionMs, m_audioDurationMs);
}

bool MessageListModel::audioIsPlaying() const { return m_audioIsPlaying; }

void MessageListModel::setAudioIsPlaying(bool playing)
{
    if (m_audioIsPlaying == playing) return;
    m_audioIsPlaying = playing;
    emit audioIsPlayingChanged();
    patchAudioPlaybackItem(m_playingAudioEventId, m_audioIsPlaying, m_audioPositionMs, m_audioDurationMs);
}

int MessageListModel::audioPositionMs() const { return m_audioPositionMs; }

void MessageListModel::setAudioPositionMs(int ms)
{
    if (m_audioPositionMs == ms) return;
    m_audioPositionMs = ms;
    emit audioPositionMsChanged();
    patchAudioPlaybackItem(m_playingAudioEventId, m_audioIsPlaying, m_audioPositionMs, m_audioDurationMs);
}

int MessageListModel::audioDurationMs() const { return m_audioDurationMs; }

void MessageListModel::setAudioDurationMs(int ms)
{
    if (m_audioDurationMs == ms) return;
    m_audioDurationMs = ms;
    emit audioDurationMsChanged();
    patchAudioPlaybackItem(m_playingAudioEventId, m_audioIsPlaying, m_audioPositionMs, m_audioDurationMs);
}

int MessageListModel::seekRequestMs() const { return m_seekRequestMs; }

void MessageListModel::setSeekRequestMs(int ms)
{
    // Deliberately no early-return-on-equal here: this is a one-shot
    // "please seek to this position" request, not steady-state UI state --
    // dragging back to a millisecond already requested (e.g. after playback
    // moved past it) must still re-fire seekRequestMsChanged().
    m_seekRequestMs = ms;
    emit seekRequestMsChanged();
}

QVariantMap MessageListModel::instagramVideoResult() const { return m_instagramVideoResult; }

QVariantMap MessageListModel::toDisplayItem(const QVariantMap &rawEvent) const
{
    QVariantMap item = rawEvent;
    qint64 ts = rawEvent.value("ts").toLongLong();
    item["timeText"] = QDateTime::fromMSecsSinceEpoch(ts).toLocalTime().toString("HH:mm");

    QString sender = rawEvent.value("sender").toString();
    // Prefer the authoritative /joined_members-backed cache (complete,
    // covers history-paginated senders too) over the per-event senderName
    // SyncEngine attaches on the fly (best-effort, lazy_load_members-
    // limited); fall back to the MXID local part if neither has it yet.
    QString senderName = m_roomMemberNames.value(sender);
    if (senderName.isEmpty()) senderName = rawEvent.value("senderName").toString();
    if (senderName.isEmpty()) {
        int colonIdx = sender.indexOf(':');
        senderName = colonIdx > 0 ? sender.mid(1, colonIdx - 1) : sender;
    }
    item["senderShort"] = senderName.isEmpty() ? sender : senderName;
    item["isGroupChat"] = m_roomMembersKnown ? m_roomIsGroupChat : rawEvent.value("isGroupChat").toBool();

    QString mediaMxc = rawEvent.value("mediaMxc").toString();
    if (!mediaMxc.isEmpty()) {
        item["mediaLocalUrl"] = m_media->resolve(mediaMxc,
                rawEvent.value("mediaKey").toString(),
                rawEvent.value("mediaIv").toString(),
                rawEvent.value("mediaHash").toString(),
                rawEvent.value("msgtype").toString() == "m.video");
    }

    QString senderAvatarMxc = m_roomMemberAvatars.value(sender);
    if (senderAvatarMxc.isEmpty()) senderAvatarMxc = rawEvent.value("senderAvatarMxc").toString();
    if (!senderAvatarMxc.isEmpty()) {
        item["senderAvatarLocalUrl"] = m_media->resolveThumbnail(senderAvatarMxc);
    }

    QString replyToEventId = rawEvent.value("replyToEventId").toString();
    if (!replyToEventId.isEmpty()) {
        // Best-effort: the parent is usually already cached (replies always
        // target an earlier event in the same room), but if it scrolled out
        // of TimelineStore's bounded cache or hasn't been paginated in yet,
        // fall back to a generic label rather than showing nothing.
        item["replyPreview"] = QString::fromUtf8("Original message");
        QVariantList roomEvents = m_store->eventsForRoom(m_roomId);
        for (int i = 0; i < roomEvents.size(); ++i) {
            QVariantMap parent = roomEvents.at(i).toMap();
            if (parent.value("eventId").toString() != replyToEventId) continue;
            QString parentSender = parent.value("sender").toString();
            QString parentName = m_roomMemberNames.value(parentSender);
            if (parentName.isEmpty()) parentName = parent.value("senderName").toString();
            if (parentName.isEmpty()) {
                int parentColonIdx = parentSender.indexOf(':');
                parentName = parentColonIdx > 0 ? parentSender.mid(1, parentColonIdx - 1) : parentSender;
            }
            QString parentBody = parent.value("body").toString();
            if (parentBody.length() > 80) parentBody = parentBody.left(80) + QString::fromUtf8("\xe2\x80\xa6");
            item["replyPreview"] = QString("%1: %2").arg(parentName.isEmpty() ? parentSender : parentName, parentBody);
            break;
        }
    }

    QString eventId = item.value("eventId").toString();
    if (!eventId.isEmpty()) {
        item["actions"] = QVariant::fromValue<QObject*>(
                rowActionsFor(eventId, sender, item.value("senderShort").toString(), previewTextFor(item),
                              item.value("msgtype").toString(), item.value("isOutgoing").toBool()));
    }
    return item;
}

QObject* MessageListModel::rowActionsFor(const QString &eventId, const QString &senderId, const QString &senderShort, const QString &bodyPreview, const QString &msgtype, bool isOutgoing) const
{
    MessageRowActions *actions = m_rowActions.value(eventId);
    if (!actions) {
        actions = new MessageRowActions(const_cast<MessageListModel*>(this));
        m_rowActions.insert(eventId, actions);
    }
    actions->setData(eventId, senderId, senderShort, bodyPreview, msgtype, isOutgoing);
    return actions;
}

void MessageListModel::setRoomId(const QString &roomId)
{
    if (m_roomId == roomId) return;
    m_roomId = roomId;
    m_model->clear();
    // Row action handlers are only ever referenced from currently-rendered
    // ListItemData -- once a room's rows are cleared above, nothing in QML
    // can still be holding one, so drop them all rather than growing this
    // hash forever as more rooms/history get viewed over a session.
    // toDisplayItem()/rowActionsFor() recreates whatever's needed the moment
    // this room (or any other) is displayed again.
    qDeleteAll(m_rowActions);
    m_rowActions.clear();
    clearReplyTarget();
    clearEditTarget();
    // Both are per-room UI state that must not leak into whichever room is
    // opened next: an edit target left staged from a different room would
    // let sendText() build an m.replace pointing at an event that doesn't
    // even exist in the new room (wrong roomId + foreign eventId); a stale
    // send-failure banner would just be misleading, showing "invio non
    // riuscito" for a room nothing was ever sent in.
    if (!m_lastSendError.isEmpty()) {
        m_lastSendError.clear();
        emit sendFailed(m_lastSendError);
    }
    setLoadingHistory(false);
    setHistoryExhausted(false);
    m_roomMemberNames.clear();
    m_roomMemberAvatars.clear();
    m_roomMembersKnown = false;
    m_roomIsGroupChat = false;
    fetchRoomMembers(roomId);

    QVariantList cached = m_store->eventsForRoom(roomId);
    QVariantList displayItems;
    for (int i = 0; i < cached.size(); ++i) {
        displayItems << toDisplayItem(cached.at(i).toMap());
    }
    m_model->append(displayItems);

    QStringList typingUsers = m_store->typingUsersForRoom(roomId);
    m_typingText = typingUsers.isEmpty() ? QString() : QString("%1 sta scrivendo...").arg(typingUsers.join(", "));

    emit roomIdChanged();
    emit typingTextChanged();

    if (m_model->size() > 0) {
        QVariantMap last = m_model->value(m_model->size() - 1).toMap();
        markRead(last.value("eventId").toString());
    }
}

void MessageListModel::fetchRoomMembers(const QString &roomId)
{
    QString path = QString("/rooms/%1/joined_members").arg(QString(QUrl::toPercentEncoding(roomId)));
    QNetworkReply *reply = m_api->apiGet(path);
    m_roomMembersReplyRoom[reply] = roomId;
    connect(reply, SIGNAL(finished()), this, SLOT(onRoomMembersReplyFinished()));
}

void MessageListModel::onRoomMembersReplyFinished()
{
    QNetworkReply *reply = qobject_cast<QNetworkReply*>(sender());
    if (!reply) return;
    QString roomId = m_roomMembersReplyRoom.take(reply);
    bool ok = false;
    QVariant parsed = MatrixApi::parseJson(reply, &ok);
    reply->deleteLater();
    if (!ok || roomId != m_roomId) return; // stale: user switched rooms before this landed

    QVariantMap joined = parsed.toMap().value("joined").toMap();
    QString selfUserId = m_api->userId();
    int otherCount = 0;
    QMapIterator<QString, QVariant> it(joined);
    while (it.hasNext()) {
        it.next();
        QString userId = it.key();
        QVariantMap info = it.value().toMap();
        QString displayName = info.value("display_name").toString();
        if (!displayName.isEmpty()) m_roomMemberNames[userId] = displayName;
        QString avatarUrl = info.value("avatar_url").toString();
        if (!avatarUrl.isEmpty()) m_roomMemberAvatars[userId] = avatarUrl;
        if (userId != selfUserId) ++otherCount;
    }
    m_roomIsGroupChat = otherCount > 1;
    m_roomMembersKnown = true;

    // Refresh everything already rendered: some items may have been built
    // with the raw-MXID fallback name before this fetch landed.
    for (int i = 0; i < m_model->size(); ++i) {
        m_model->replace(i, toDisplayItem(m_model->value(i).toMap()));
    }
}

void MessageListModel::onEventAppended(const QString &roomId, const QVariantMap &event)
{
    if (roomId != m_roomId) return;
    m_model->append(toDisplayItem(event));
    if (!event.value("isOutgoing").toBool()) {
        markRead(event.value("eventId").toString());
    }
}

void MessageListModel::onHistoryPrepended(const QString &roomId, const QVariantList &events)
{
    if (roomId != m_roomId || events.isEmpty()) return;
    QVariantList displayItems;
    for (int i = 0; i < events.size(); ++i) displayItems << toDisplayItem(events.at(i).toMap());
    m_model->insert(0, displayItems);
}

void MessageListModel::onEventUpdated(const QString &roomId, const QVariantMap &event)
{
    if (roomId != m_roomId) return;
    QString eventId = event.value("eventId").toString();
    for (int i = 0; i < m_model->size(); ++i) {
        QVariantMap existing = m_model->value(i).toMap();
        if (existing.value("eventId").toString() == eventId) {
            QVariantMap merged = toDisplayItem(event);
            if (existing.contains("readBy")) merged["readBy"] = existing.value("readBy");
            m_model->replace(i, merged);
            break;
        }
    }
}

void MessageListModel::onTypingChanged(const QString &roomId, const QStringList &userIds)
{
    if (roomId != m_roomId) return;
    m_typingText = userIds.isEmpty() ? QString() : QString("%1 sta scrivendo...").arg(userIds.join(", "));
    emit typingTextChanged();
}

void MessageListModel::onReceiptChanged(const QString &roomId, const QString &eventId, const QStringList &userIds)
{
    if (roomId != m_roomId) return;
    for (int i = 0; i < m_model->size(); ++i) {
        QVariantMap item = m_model->value(i).toMap();
        if (item.value("eventId").toString() == eventId) {
            item["readBy"] = userIds;
            m_model->replace(i, item);
            break;
        }
    }
}

void MessageListModel::onReactionsChanged(const QString &roomId, const QString &eventId, const QVariantList &reactions)
{
    if (roomId != m_roomId) return;
    for (int i = 0; i < m_model->size(); ++i) {
        QVariantMap item = m_model->value(i).toMap();
        if (item.value("eventId").toString() == eventId) {
            item["reactions"] = reactions;
            m_model->replace(i, item);
            break;
        }
    }
}

void MessageListModel::onMediaReady(const QString &mxcUri, const QString &localFileUrl)
{
    for (int i = 0; i < m_model->size(); ++i) {
        QVariantMap item = m_model->value(i).toMap();
        if (item.value("mediaMxc").toString() == mxcUri) {
            item["mediaLocalUrl"] = localFileUrl;
            m_model->replace(i, item);
        }
    }
}

void MessageListModel::onMediaFailed(const QString &mxcUri)
{
    for (int i = 0; i < m_model->size(); ++i) {
        QVariantMap item = m_model->value(i).toMap();
        if (item.value("mediaMxc").toString() == mxcUri) {
            item["mediaDownloadFailed"] = true;
            m_model->replace(i, item);
        }
    }
}

void MessageListModel::onThumbnailReady(const QString &mxcUri, const QString &localFileUrl)
{
    for (int i = 0; i < m_model->size(); ++i) {
        QVariantMap item = m_model->value(i).toMap();
        if (item.value("senderAvatarMxc").toString() == mxcUri) {
            item["senderAvatarLocalUrl"] = localFileUrl;
            m_model->replace(i, item);
        }
    }
}

void MessageListModel::onInstagramVideoReady(const QString &instagramUrl, const QString &localFileUrl)
{
    QString eventId;
    for (int i = 0; i < m_model->size(); ++i) {
        QVariantMap item = m_model->value(i).toMap();
        if (item.value("instagramUrl").toString() != instagramUrl) continue;
        // Reuses mediaLocalUrl -- the same field a real m.video message's
        // player already reads from -- so this row behaves identically to
        // one from here on, if anything ever re-reads it.
        item["mediaLocalUrl"] = localFileUrl;
        m_model->replace(i, item);
        eventId = item.value("eventId").toString();
    }
    m_instagramVideoResult["ok"] = true;
    m_instagramVideoResult["eventId"] = eventId;
    m_instagramVideoResult["localFileUrl"] = localFileUrl;
    emit instagramVideoResultChanged();
}

void MessageListModel::onInstagramVideoFailed(const QString &instagramUrl)
{
    QString eventId;
    for (int i = 0; i < m_model->size(); ++i) {
        const QVariantMap item = m_model->value(i).toMap();
        if (item.value("instagramUrl").toString() == instagramUrl) {
            eventId = item.value("eventId").toString();
            break;
        }
    }
    m_instagramVideoResult["ok"] = false;
    m_instagramVideoResult["eventId"] = eventId;
    m_instagramVideoResult["localFileUrl"] = QString();
    emit instagramVideoResultChanged();
}

QVariantMap MessageListModel::replyTarget() const { return m_replyTarget; }

bool MessageListModel::hasReplyTarget() const { return !m_replyTarget.value("eventId").toString().isEmpty(); }

void MessageListModel::setReplyTarget(const QString &eventId, const QString &senderId, const QString &senderShort, const QString &bodyPreview)
{
    clearEditTarget();
    m_replyTarget["eventId"] = eventId;
    m_replyTarget["senderId"] = senderId;
    m_replyTarget["senderShort"] = senderShort;
    m_replyTarget["bodyPreview"] = bodyPreview;
    emit replyTargetChanged();
}

void MessageListModel::clearReplyTarget()
{
    if (m_replyTarget.isEmpty()) return;
    m_replyTarget.clear();
    emit replyTargetChanged();
}

QVariantMap MessageListModel::editTarget() const { return m_editTarget; }

bool MessageListModel::hasEditTarget() const { return !m_editTarget.value("eventId").toString().isEmpty(); }

void MessageListModel::setEditTarget(const QString &eventId, const QString &body)
{
    // Mutually exclusive with replying -- editing replaces the composer's
    // whole content, so any staged reply no longer makes sense once the
    // user picks "Edit" instead.
    clearReplyTarget();
    m_editTarget["eventId"] = eventId;
    m_editTarget["body"] = body;
    emit editTargetChanged();
}

void MessageListModel::clearEditTarget()
{
    if (m_editTarget.isEmpty()) return;
    m_editTarget.clear();
    emit editTargetChanged();
}

void MessageListModel::sendReaction(const QString &targetEventId, const QString &key)
{
    if (m_roomId.isEmpty() || targetEventId.isEmpty()) return;

    QVariantMap relatesTo;
    relatesTo["rel_type"] = "m.annotation";
    relatesTo["event_id"] = targetEventId;
    relatesTo["key"] = key;
    QVariantMap content;
    content["m.relates_to"] = relatesTo;

    QString txnId = m_api->nextTxnId();

    if (m_olmCrypto && m_olmCrypto->isRoomEncrypted(m_roomId)) {
        m_olmCrypto->encryptAndSend(m_roomId, content, txnId, "m.reaction");
        return;
    }

    QString path = QString("/rooms/%1/send/m.reaction/%2")
            .arg(QString(QUrl::toPercentEncoding(m_roomId)))
            .arg(QString(QUrl::toPercentEncoding(txnId)));
    QNetworkReply *reply = m_api->apiPut(path, content);
    connect(reply, SIGNAL(finished()), this, SLOT(onGenericActionReplyFinished()));
}

void MessageListModel::redactMessage(const QString &eventId)
{
    if (m_roomId.isEmpty() || eventId.isEmpty()) return;

    // m.room.redaction is never encrypted, even in an E2EE room -- the
    // homeserver has to be able to act on it regardless of room encryption.
    QString txnId = m_api->nextTxnId();
    QString path = QString("/rooms/%1/redact/%2/%3")
            .arg(QString(QUrl::toPercentEncoding(m_roomId)))
            .arg(QString(QUrl::toPercentEncoding(eventId)))
            .arg(QString(QUrl::toPercentEncoding(txnId)));
    QNetworkReply *reply = m_api->apiPut(path, QVariantMap());
    connect(reply, SIGNAL(finished()), this, SLOT(onGenericActionReplyFinished()));
}

void MessageListModel::onGenericActionReplyFinished()
{
    QNetworkReply *reply = qobject_cast<QNetworkReply*>(sender());
    if (!reply) return;

    bool ok = false;
    QVariant parsed = MatrixApi::parseJson(reply, &ok);
    reply->deleteLater();

    if (!ok || !parsed.toMap().contains("event_id")) {
        QVariantMap errMap = parsed.toMap();
        QString detail = errMap.value("error").toString();
        QString message = detail.isEmpty() ? "Operation failed." : detail;
        m_lastSendError = message;
        emit sendFailed(message);
    }
}

void MessageListModel::sendText(const QString &body)
{
    if (m_roomId.isEmpty() || body.trimmed().isEmpty()) return;

    setSending(true);
    sendTypingState(false);

    QVariantMap content;
    content["msgtype"] = "m.text";

    QString editEventId = m_editTarget.value("eventId").toString();
    QString replyEventId = m_replyTarget.value("eventId").toString();

    if (!editEventId.isEmpty()) {
        // m.replace: the same content shape as a normal message, PLUS
        // m.new_content carrying the real new text and m.relates_to pointing
        // at the original event. body/formatted_body at the top level are a
        // fallback preview for clients that don't understand edits (per
        // spec convention, prefixed with "* ").
        QVariantMap newContent;
        newContent["msgtype"] = "m.text";
        newContent["body"] = body;
        content["m.new_content"] = newContent;

        QVariantMap relatesTo;
        relatesTo["rel_type"] = "m.replace";
        relatesTo["event_id"] = editEventId;
        content["m.relates_to"] = relatesTo;

        content["body"] = "* " + body;
        clearEditTarget();
    } else if (!replyEventId.isEmpty()) {
        // Matches a real reply event this exact Beeper/Element combination
        // produced (captured directly from the wire and compared against):
        // plain body (the reply text alone, no "> quoted" prefix and no
        // format/formatted_body at all), m.relates_to.m.in_reply_to, and
        // m.mentions.user_ids naming the parent's sender -- the current
        // "intentional mentions" convention, not just the older rich-reply
        // fallback. The original hand-built <mx-reply> HTML fallback this
        // replaced was closer to an older Element convention that isn't
        // what's actually produced here, and evidently wasn't being treated
        // as a real reply as a result.
        QString senderId = m_replyTarget.value("senderId").toString();

        QVariantMap inReplyTo;
        inReplyTo["event_id"] = replyEventId;
        QVariantMap relatesTo;
        relatesTo["m.in_reply_to"] = inReplyTo;
        content["m.relates_to"] = relatesTo;

        if (!senderId.isEmpty()) {
            QVariantMap mentions;
            mentions["user_ids"] = QVariantList() << senderId;
            content["m.mentions"] = mentions;
        }

        content["body"] = body;
        clearReplyTarget();
    } else {
        content["body"] = body;
    }

    QString txnId = m_api->nextTxnId();

    if (m_olmCrypto && m_olmCrypto->isRoomEncrypted(m_roomId)) {
        // Asynchronous: onEncryptedSendSucceeded()/onEncryptedSendFailed()
        // clear m_sending once the whole encrypt-and-send chain finishes.
        m_olmCrypto->encryptAndSend(m_roomId, content, txnId);
        return;
    }

    QString path = QString("/rooms/%1/send/m.room.message/%2")
            .arg(QString(QUrl::toPercentEncoding(m_roomId)))
            .arg(QString(QUrl::toPercentEncoding(txnId)));

    QNetworkReply *reply = m_api->apiPut(path, content);
    connect(reply, SIGNAL(finished()), this, SLOT(onSendReplyFinished()));
}

void MessageListModel::onEncryptedSendSucceeded(const QString &txnId)
{
    Q_UNUSED(txnId);
    setSending(false);
}

void MessageListModel::onEncryptedSendFailed(const QString &txnId, const QString &error)
{
    Q_UNUSED(txnId);
    setSending(false);
    m_lastSendError = error;
    emit sendFailed(error);
}

void MessageListModel::onSendReplyFinished()
{
    QNetworkReply *reply = qobject_cast<QNetworkReply*>(sender());
    setSending(false);
    if (!reply) return;

    bool ok = false;
    QVariant parsed = MatrixApi::parseJson(reply, &ok);
    reply->deleteLater();

    if (!ok || !parsed.toMap().contains("event_id")) {
        // Include the homeserver's own error text/code when there is one
        // (a parseable JSON body means the request DID reach the server --
        // ok stays true even for a Matrix-level error response like 400/403
        // -- it's just missing event_id) rather than a single generic
        // message covering both "never reached the server" and "server
        // rejected it" cases, which previously had no visible UI anyway
        // (see lastSendError's Q_PROPERTY doc comment).
        QVariantMap errMap = parsed.toMap();
        QString detail = errMap.value("error").toString();
        QString errcode = errMap.value("errcode").toString();
        QString message = "Send failed.";
        if (!detail.isEmpty()) {
            message += QString(" %1%2").arg(detail, errcode.isEmpty() ? QString() : QString(" (%1)").arg(errcode));
        } else if (!ok) {
            message += " Check your connection.";
        }
        m_lastSendError = message;
        emit sendFailed(message);
    }
    // On success the message arrives shortly through SyncEngine's own /sync
    // stream and is appended by onEventAppended() like any other event.
}

void MessageListModel::sendImage(const QString &localFilePath)
{
    if (m_roomId.isEmpty() || localFilePath.isEmpty()) return;
    PendingUpload pending;
    pending.roomId = m_roomId;
    pending.msgtype = "m.image";
    pending.durationMs = 0;
    m_pendingUploads[localFilePath] = pending;
    m_media->upload(localFilePath);
}

void MessageListModel::sendAudio(const QString &localFilePath, int durationMs)
{
    if (m_roomId.isEmpty() || localFilePath.isEmpty()) return;
    PendingUpload pending;
    pending.roomId = m_roomId;
    pending.msgtype = "m.audio";
    pending.durationMs = durationMs;
    m_pendingUploads[localFilePath] = pending;
    m_media->upload(localFilePath);
}

void MessageListModel::onUploadFinished(const QString &localFilePath, const QString &mxcUri, const QString &mimeType, bool ok)
{
    if (!m_pendingUploads.contains(localFilePath)) return;
    PendingUpload pending = m_pendingUploads.take(localFilePath);
    if (!ok) {
        if (pending.roomId == m_roomId) {
            m_lastSendError = pending.msgtype == "m.audio"
                    ? "Failed to send audio."
                    : "Failed to upload image.";
            emit sendFailed(m_lastSendError);
        }
        return;
    }

    QVariantMap info;
    info["mimetype"] = mimeType;
    info["size"] = (qlonglong)QFileInfo(localFilePath).size();
    if (pending.msgtype == "m.audio" && pending.durationMs > 0) {
        info["duration"] = pending.durationMs;
    }

    QVariantMap content;
    content["msgtype"] = pending.msgtype;
    content["body"] = QFileInfo(localFilePath).fileName();
    content["url"] = mxcUri;
    content["info"] = info;

    QString txnId = m_api->nextTxnId();

    if (m_olmCrypto && m_olmCrypto->isRoomEncrypted(pending.roomId)) {
        // NOTE: this sends the message JSON itself Megolm-encrypted, same as
        // text, but the uploaded media BLOB above is not -- outbound
        // encrypted-file upload (client-side AES-256-CTR before uploading,
        // per the same EncryptedFile format resolve()/decryptFile() already
        // consume on the receiving end) isn't implemented yet. Pre-existing
        // gap (sendImage had it before this), not introduced here; flagged
        // rather than silently shipped.
        if (pending.roomId == m_roomId) setSending(true);
        m_olmCrypto->encryptAndSend(pending.roomId, content, txnId);
        return;
    }

    QString path = QString("/rooms/%1/send/m.room.message/%2")
            .arg(QString(QUrl::toPercentEncoding(pending.roomId)))
            .arg(QString(QUrl::toPercentEncoding(txnId)));

    QNetworkReply *reply = m_api->apiPut(path, content);
    connect(reply, SIGNAL(finished()), this, SLOT(onSendReplyFinished()));
}

void MessageListModel::sendTypingState(bool typing)
{
    if (m_roomId.isEmpty()) return;
    if (typing == m_typingActiveSent) {
        if (typing) m_typingStopTimer->start(kTypingStopDelayMs);
        return;
    }
    m_typingActiveSent = typing;

    QVariantMap content;
    content["typing"] = typing;
    if (typing) content["timeout"] = kTypingStopDelayMs + 2000;

    QString path = QString("/rooms/%1/typing/%2")
            .arg(QString(QUrl::toPercentEncoding(m_roomId)))
            .arg(QString(QUrl::toPercentEncoding(m_api->userId())));

    QNetworkReply *reply = m_api->apiPut(path, content);
    connect(reply, SIGNAL(finished()), reply, SLOT(deleteLater()));

    if (typing) m_typingStopTimer->start(kTypingStopDelayMs);
    else m_typingStopTimer->stop();
}

void MessageListModel::setTypingActive(bool active)
{
    sendTypingState(active);
}

void MessageListModel::onTypingStopTimeout()
{
    sendTypingState(false);
}

void MessageListModel::markRead(const QString &eventId)
{
    if (m_roomId.isEmpty() || eventId.isEmpty()) return;

    QString path = QString("/rooms/%1/receipt/m.read/%2")
            .arg(QString(QUrl::toPercentEncoding(m_roomId)))
            .arg(QString(QUrl::toPercentEncoding(eventId)));

    QNetworkReply *reply = m_api->apiPost(path, QVariantMap());
    connect(reply, SIGNAL(finished()), reply, SLOT(deleteLater()));
}

void MessageListModel::loadOlderMessages()
{
    if (m_roomId.isEmpty() || m_loadingHistory || m_historyExhausted) return;

    QString from = m_store->prevBatchFor(m_roomId);
    if (from.isEmpty()) {
        setHistoryExhausted(true);
        return;
    }

    setLoadingHistory(true);

    QString path = QString("/rooms/%1/messages").arg(QString(QUrl::toPercentEncoding(m_roomId)));
    QVariantMap query;
    query["dir"] = "b";
    query["limit"] = QString::number(kHistoryPageSize);
    query["from"] = from;

    QNetworkReply *reply = m_api->apiGet(path, query);
    reply->setProperty("beport_history_room", m_roomId);
    connect(reply, SIGNAL(finished()), this, SLOT(onHistoryReplyFinished()));
}

void MessageListModel::onHistoryReplyFinished()
{
    QNetworkReply *reply = qobject_cast<QNetworkReply*>(sender());
    setLoadingHistory(false);
    if (!reply) return;

    QString roomId = reply->property("beport_history_room").toString();
    bool ok = false;
    QVariant parsed = MatrixApi::parseJson(reply, &ok);
    reply->deleteLater();
    if (!ok) return;

    QVariantMap map = parsed.toMap();
    QVariantList chunk = map.value("chunk").toList();
    QString end = map.value("end").toString();

    if (chunk.isEmpty() || end.isEmpty()) {
        if (roomId == m_roomId) setHistoryExhausted(true);
        return;
    }

    m_store->setPrevBatch(roomId, end);

    // "chunk" for dir=b is newest-first (closest to "from" first); reverse
    // to oldest-first so prependHistory() inserts in correct reading order.
    QVariantList built;
    for (int i = chunk.size() - 1; i >= 0; --i) {
        QVariantMap outEvent;
        if (buildHistoryEvent(chunk.at(i).toMap(), &outEvent)) built << outEvent;
    }
    if (!built.isEmpty()) m_store->prependHistory(roomId, built);

    // Reactions are applied after prependHistory() so their target events
    // (if in this same page) are already in the store to attach to.
    for (int i = 0; i < chunk.size(); ++i) {
        QString targetEventId, key, sender, reactionEventId;
        if (extractHistoryReaction(chunk.at(i).toMap(), &targetEventId, &key, &sender, &reactionEventId)) {
            m_store->onReactionAdded(roomId, targetEventId, key, sender, reactionEventId);
        }
    }
}

bool MessageListModel::extractHistoryReaction(const QVariantMap &ev, QString *targetEventId, QString *key, QString *sender, QString *reactionEventId) const
{
    QString type = ev.value("type").toString();
    QVariantMap content = ev.value("content").toMap();

    if (type == "m.room.encrypted") {
        QString algorithm = content.value("algorithm").toString();
        QString sessionId = content.value("session_id").toString();
        QString ciphertext = content.value("ciphertext").toString();
        QString plaintextJson;
        if (!m_keyBackup || algorithm != "m.megolm.v1.aes-sha2"
                || !m_keyBackup->decrypt(m_roomId, sessionId, ciphertext, &plaintextJson)) {
            return false;
        }
        JsonDataAccess jda;
        QVariant inner = jda.loadFromBuffer(plaintextJson.toUtf8());
        if (jda.hasError()) return false;
        type = inner.toMap().value("type").toString();
        content = inner.toMap().value("content").toMap();
    }

    if (!parseReaction(type, content, targetEventId, key)) return false;
    *sender = ev.value("sender").toString();
    *reactionEventId = ev.value("event_id").toString();
    return true;
}

bool MessageListModel::buildHistoryEvent(const QVariantMap &ev, QVariantMap *outEvent) const
{
    QString type = ev.value("type").toString();
    if (type != "m.room.message" && type != "m.room.encrypted" && type != "m.sticker") return false;

    QString eventId = ev.value("event_id").toString();
    QString sender = ev.value("sender").toString();
    qint64 ts = ev.value("origin_server_ts").toLongLong();
    bool isOutgoing = (sender == m_api->userId());
    QVariantMap content = ev.value("content").toMap();

    // Same "already redacted before we ever saw it" case SyncEngine handles
    // for live sync -- history-paginated messages carry the exact same
    // unsigned.redacted_because shape, content stripped to {} server-side.
    if (ev.value("unsigned").toMap().contains("redacted_because")) {
        (*outEvent)["eventId"] = eventId;
        (*outEvent)["sender"] = sender;
        (*outEvent)["msgtype"] = "m.text";
        (*outEvent)["body"] = QString::fromUtf8("Message deleted");
        (*outEvent)["ts"] = ts;
        (*outEvent)["isOutgoing"] = isOutgoing;
        (*outEvent)["isRedacted"] = true;
        return true;
    }

    if (type == "m.room.encrypted") {
        QString algorithm = content.value("algorithm").toString();
        QString sessionId = content.value("session_id").toString();
        QString ciphertext = content.value("ciphertext").toString();

        QString innerType;
        QString plaintextJson;
        if (m_keyBackup && algorithm == "m.megolm.v1.aes-sha2"
                && m_keyBackup->decrypt(m_roomId, sessionId, ciphertext, &plaintextJson)) {
            JsonDataAccess jda;
            QVariant inner = jda.loadFromBuffer(plaintextJson.toUtf8());
            innerType = inner.toMap().value("type").toString();
            if (!jda.hasError() && (innerType == "m.room.message" || innerType == "m.sticker")) {
                content = inner.toMap().value("content").toMap();
            } else {
                content = QVariantMap();
            }
        } else {
            content = QVariantMap();
        }
        if (innerType == "m.sticker") type = "m.sticker"; // reuse the plaintext m.sticker path below

        if (content.isEmpty()) {
            (*outEvent)["eventId"] = eventId;
            (*outEvent)["sender"] = sender;
            (*outEvent)["msgtype"] = "m.text";
            (*outEvent)["body"] = QString::fromUtf8("\xf0\x9f\x94\x92 Encrypted message (not supported)");
            (*outEvent)["ts"] = ts;
            (*outEvent)["isOutgoing"] = isOutgoing;
            return true;
        }
    }

    QString msgtype = type == "m.sticker" ? QString("m.sticker") : content.value("msgtype").toString();
    QString body = content.value("body").toString();
    QString replyToEventId = stripReplyFallback(content, &body);
    (*outEvent)["eventId"] = eventId;
    (*outEvent)["sender"] = sender;
    (*outEvent)["msgtype"] = msgtype;
    (*outEvent)["body"] = body;
    (*outEvent)["ts"] = ts;
    (*outEvent)["isOutgoing"] = isOutgoing;
    if (!replyToEventId.isEmpty()) (*outEvent)["replyToEventId"] = replyToEventId;
    if (msgtype == "m.image" || msgtype == "m.file" || msgtype == "m.video" || msgtype == "m.audio" || msgtype == "m.sticker") {
        extractMediaFields(content, outEvent);
    }
    return true;
}
