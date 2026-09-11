#ifndef MESSAGELISTMODEL_HPP_
#define MESSAGELISTMODEL_HPP_

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <QVariantList>
#include <QHash>

class MatrixApi;
class TimelineStore;
class MediaManager;
class KeyBackupManager;
class OlmCryptoManager;
class QTimer;
class QNetworkReply;

namespace bb { namespace cascades { class ArrayDataModel; } }

class MessageListModel;

// One tiny QObject per row, stashed in that row's own QVariantMap under the
// "actions" key (item["actions"] = QVariant::fromValue<QObject*>(...)) so
// it's reachable from QML as ListItemData.actions -- and ListItemData is the
// ONE thing a ListItemComponent delegate's contextActions can actually see
// (confirmed repeatedly: neither context properties like messageListModel
// nor same-document ids like messageView resolve from inside a delegate's
// isolated context, but ListItemData itself always does, since it's how the
// row's fields render in the first place). The two previous long-press
// attempts both tried to have the delegate's own JS *reach out* to
// messageListModel or a Page-scope function by name, which needs scope
// resolution the delegate doesn't have; this one instead reaches *in*
// through data the delegate already legitimately holds, and the actual
// jump back to MessageListModel happens in C++ (this object holds a real
// pointer to it), never in QML/JS scope lookup at all.
class MessageRowActions : public QObject
{
    Q_OBJECT
public:
    explicit MessageRowActions(MessageListModel *owner);
    void setData(const QString &eventId, const QString &senderId, const QString &senderShort, const QString &bodyPreview, const QString &msgtype, bool isOutgoing);
    Q_INVOKABLE void reply();
    // Sends a heart reaction (m.reaction, rel_type m.annotation) to this row.
    Q_INVOKABLE void like();
    // Stages this row as the active edit target (see MessageListModel's
    // editTarget) -- main.qml's property-mirror watcher then fills the
    // composer with m_bodyPreview, and the next sendText() becomes an
    // m.replace of this event instead of a new message. No-ops for anything
    // other than the user's own m.text messages: ActionItem has no QML
    // `visible` property to hide this menu entry conditionally (confirmed
    // against the AbstractActionItem header -- same class of trap as
    // Button.background), and m_bodyPreview is just a generic placeholder
    // ("Foto", "Video", ...) for non-text msgtypes, not real editable
    // content -- letting this through used to send a real m.replace that
    // overwrote e.g. a photo message with the literal text "Foto". Editing
    // another sender's message would also just be rejected homeserver-side
    // (edits must come from the original sender), so isOutgoing is checked
    // too rather than relying on that rejection.
    Q_INVOKABLE void edit();
    // Copies m_bodyPreview to the system clipboard. No-ops for non-m.text
    // messages for the same "m_bodyPreview is a placeholder, not real
    // content" reason as edit() above.
    Q_INVOKABLE void copy();
    // Named remove(), not delete() -- delete is a C++ keyword. Sends
    // m.room.redaction for this event.
    Q_INVOKABLE void remove();

private:
    MessageListModel *m_owner;
    QString m_eventId;
    QString m_senderId;
    QString m_senderShort;
    QString m_bodyPreview;
    QString m_msgtype;
    bool m_isOutgoing;
};

// Timeline for whichever single room is currently open in the conversation
// page. Rebinding roomId swaps in that room's cached history from
// TimelineStore without losing anything the other rooms already received.
class MessageListModel : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString roomId READ roomId WRITE setRoomId NOTIFY roomIdChanged)
    Q_PROPERTY(QObject* model READ model CONSTANT)
    Q_PROPERTY(QString typingText READ typingText NOTIFY typingTextChanged)
    Q_PROPERTY(bool sending READ isSending NOTIFY sendingChanged)
    // Text of the most recent sendFailed() signal, so QML can show it
    // without wiring an explicit signal connection (unverified whether
    // Connections{} works reliably in this Cascades/QML1 build -- the
    // established, proven pattern elsewhere in this file is a plain
    // property + onXChanged instead). Previously sendFailed() had NO
    // listener at all in main.qml, so every send failure (network error,
    // encryption error, server rejection) was completely silent -- the
    // composer clears optimistically regardless of the outcome, so with no
    // error surfaced a failed send looked identical to a successful one.
    Q_PROPERTY(QString lastSendError READ lastSendError NOTIFY sendFailed)
    Q_PROPERTY(bool loadingHistory READ isLoadingHistory NOTIFY loadingHistoryChanged)
    Q_PROPERTY(bool historyExhausted READ isHistoryExhausted NOTIFY historyExhaustedChanged)
    // Which m.audio message (by eventId) the shared chatAudioPlayer in
    // main.qml currently has loaded, and whether it's actively playing.
    // Plain read/write UI state with no business logic behind it -- it
    // exists here (a context property, reachable from anywhere) rather than
    // as a local `id` on the player itself, because a ListItemComponent
    // delegate's bindings couldn't reliably resolve sibling `id`s back on
    // the Page (chatAudioPlayer.mediaState etc. silently failed to bind),
    // while context properties like this one work from any nesting depth.
    Q_PROPERTY(QString playingAudioEventId READ playingAudioEventId WRITE setPlayingAudioEventId NOTIFY playingAudioEventIdChanged)
    Q_PROPERTY(bool audioIsPlaying READ audioIsPlaying WRITE setAudioIsPlaying NOTIFY audioIsPlayingChanged)
    // Live position/duration (ms) of the shared player's current track, kept
    // in sync from main.qml's chatAudioPlayer.onPositionChanged/
    // onDurationChanged (Page scope) so the delegate's scrub bar can read
    // them. seekRequestMs is the reverse direction: the delegate writes a
    // target position here when the user drags the bar, and a Connections
    // block at Page scope (which CAN reach chatAudioPlayer) performs the
    // actual chatAudioPlayer.seekTime() call.
    Q_PROPERTY(int audioPositionMs READ audioPositionMs WRITE setAudioPositionMs NOTIFY audioPositionMsChanged)
    Q_PROPERTY(int audioDurationMs READ audioDurationMs WRITE setAudioDurationMs NOTIFY audioDurationMsChanged)
    Q_PROPERTY(int seekRequestMs READ seekRequestMs WRITE setSeekRequestMs NOTIFY seekRequestMsChanged)
    // Last result of tapping an Instagram Reel thumbnail to fetch/play it as
    // a real video (see MediaManager::fetchInstagramVideo()): a QVariantMap
    // {"eventId", "localFileUrl", "ok"} set right before
    // instagramVideoResultChanged() fires each time, watched at Page scope
    // in main.qml (same "property mirror" trick as seekRequestMs above,
    // since a delegate can't reach navigationPane to push the video page
    // itself, and this needs to fire an action, not just update a value a
    // delegate could bind to).
    Q_PROPERTY(QVariantMap instagramVideoResult READ instagramVideoResult NOTIFY instagramVideoResultChanged)
    // The message currently staged as a reply target, or an empty map if
    // none -- {"eventId", "senderShort", "bodyPreview"}. Set via
    // setReplyTarget() (called from a message bubble's long-press context
    // action in main.qml's contextActions), read by the composer's
    // "replying to ..." banner, and consumed (then cleared) by sendText().
    Q_PROPERTY(QVariantMap replyTarget READ replyTarget NOTIFY replyTargetChanged)
    // Plain bool mirror of "replyTarget is non-empty", for the banner's
    // visible: binding. Exists because binding directly to a nested
    // sub-property of a QVariantMap-typed Q_PROPERTY (replyTarget.eventId)
    // turned out not to be a reliable QML1 "became empty, so hide" signal on
    // this Cascades build -- the banner stayed shown after clearReplyTarget().
    // A plain top-level bool re-evaluates correctly instead.
    Q_PROPERTY(bool hasReplyTarget READ hasReplyTarget NOTIFY replyTargetChanged)
    // Same shape/pattern as replyTarget above, for long-press "Edit":
    // {"eventId", "body"} of the message currently staged for editing, or an
    // empty map if none. Consumed (then cleared) by sendText(), which builds
    // an m.replace event instead of a new message when this is set.
    Q_PROPERTY(QVariantMap editTarget READ editTarget NOTIFY editTargetChanged)
    Q_PROPERTY(bool hasEditTarget READ hasEditTarget NOTIFY editTargetChanged)

public:
    MessageListModel(MatrixApi *api, TimelineStore *store, MediaManager *media, KeyBackupManager *keyBackup, OlmCryptoManager *olmCrypto, QObject *parent = 0);
    virtual ~MessageListModel();

    QString roomId() const;
    void setRoomId(const QString &roomId);
    QObject* model() const;
    QString typingText() const;
    bool isSending() const;
    QString lastSendError() const;
    bool isLoadingHistory() const;
    bool isHistoryExhausted() const;
    QString playingAudioEventId() const;
    void setPlayingAudioEventId(const QString &eventId);
    bool audioIsPlaying() const;
    void setAudioIsPlaying(bool playing);
    int audioPositionMs() const;
    void setAudioPositionMs(int ms);
    int audioDurationMs() const;
    void setAudioDurationMs(int ms);
    int seekRequestMs() const;
    void setSeekRequestMs(int ms);
    QVariantMap instagramVideoResult() const;
    QVariantMap replyTarget() const;
    bool hasReplyTarget() const;
    // eventId/senderId identify the message and its sender's full MXID
    // (needed for the matrix.to permalinks in the rich-reply
    // formatted_body -- see sendText()); senderShort/bodyPreview are
    // display-only, for the composer's "replying to ..." banner.
    Q_INVOKABLE void setReplyTarget(const QString &eventId, const QString &senderId, const QString &senderShort, const QString &bodyPreview);
    Q_INVOKABLE void clearReplyTarget();
    QVariantMap editTarget() const;
    bool hasEditTarget() const;
    Q_INVOKABLE void setEditTarget(const QString &eventId, const QString &body);
    Q_INVOKABLE void clearEditTarget();
    // Sends key ("❤️" for the long-press "like") as an m.reaction
    // annotating targetEventId.
    void sendReaction(const QString &targetEventId, const QString &key);
    // Sends m.room.redaction for eventId -- always plaintext, per spec,
    // even in an encrypted room.
    void redactMessage(const QString &eventId);

public slots:
    void sendText(const QString &body);
    void sendImage(const QString &localFilePath);
    // durationMs is the recording length reported by AudioRecorder (0 if
    // unknown); sent as content.info.duration, per the m.audio msgtype.
    void sendAudio(const QString &localFilePath, int durationMs);
    void setTypingActive(bool active);
    void markRead(const QString &eventId);
    // Fetches the next (older) page of this room's history via
    // GET /rooms/{roomId}/messages?dir=b, decrypting what it can, and
    // prepends the results above what's already shown.
    void loadOlderMessages();

signals:
    void roomIdChanged();
    void typingTextChanged();
    void sendingChanged();
    void sendFailed(const QString &error);
    void loadingHistoryChanged();
    void historyExhaustedChanged();
    void playingAudioEventIdChanged();
    void audioIsPlayingChanged();
    void audioPositionMsChanged();
    void audioDurationMsChanged();
    void seekRequestMsChanged();
    void instagramVideoResultChanged();
    void replyTargetChanged();
    void editTargetChanged();

private slots:
    void onEventAppended(const QString &roomId, const QVariantMap &event);
    void onEventUpdated(const QString &roomId, const QVariantMap &event);
    void onHistoryPrepended(const QString &roomId, const QVariantList &events);
    void onTypingChanged(const QString &roomId, const QStringList &userIds);
    void onReceiptChanged(const QString &roomId, const QString &eventId, const QStringList &userIds);
    void onReactionsChanged(const QString &roomId, const QString &eventId, const QVariantList &reactions);
    void onMediaReady(const QString &mxcUri, const QString &localFileUrl);
    void onMediaFailed(const QString &mxcUri);
    void onThumbnailReady(const QString &mxcUri, const QString &localFileUrl);
    void onInstagramVideoReady(const QString &instagramUrl, const QString &localFileUrl);
    void onInstagramVideoFailed(const QString &instagramUrl);
    void onUploadFinished(const QString &localFilePath, const QString &mxcUri, const QString &mimeType, bool ok);
    void onSendReplyFinished();
    void onTypingStopTimeout();
    void onHistoryReplyFinished();
    void onEncryptedSendSucceeded(const QString &txnId);
    void onEncryptedSendFailed(const QString &txnId, const QString &error);
    void onRoomMembersReplyFinished();
    // Shared completion handler for sendReaction()/redactMessage(): unlike
    // sendText()'s onSendReplyFinished(), this never touches the "sending"
    // property (that's the composer's own spinner, not appropriate for a
    // background long-press action) -- it only surfaces a failure, if any,
    // through the same sendFailed()/lastSendError the composer banner reads.
    void onGenericActionReplyFinished();

private:
    QVariantMap toDisplayItem(const QVariantMap &rawEvent) const;
    // Converts one raw /messages "chunk" event (m.room.message or
    // m.room.encrypted) into the same shape SyncEngine emits for live
    // events. Returns false for event types that don't render as a message
    // (membership changes, etc.), leaving *outEvent untouched.
    bool buildHistoryEvent(const QVariantMap &rawEvent, QVariantMap *outEvent) const;
    // Recognizes a raw /messages chunk entry as an m.reaction, plaintext or
    // decrypted from m.room.encrypted alike. False (out-params untouched) for
    // anything else.
    bool extractHistoryReaction(const QVariantMap &rawEvent, QString *targetEventId, QString *key, QString *sender, QString *reactionEventId) const;
    // Fetches the CURRENT room's full member list (GET /joined_members),
    // which -- unlike the per-event senderName/senderAvatarMxc SyncEngine
    // attaches on the fly (best-effort, subject to lazy_load_members only
    // ever showing senders who've actually posted) -- is complete and
    // accurate for everyone actually in the room, including history-
    // paginated messages from a sender we haven't seen a live event from
    // yet this session. Refreshes every item already in the model once it
    // lands, since some may have rendered with the raw-MXID fallback first.
    void fetchRoomMembers(const QString &roomId);
    // Writes current playback state into the matching row's own item (see
    // .cpp for why: the delegate can't reach this object's Q_PROPERTYs
    // directly). No-op if eventId is empty (nothing loaded/stopped).
    void patchAudioPlaybackItem(const QString &eventId, bool playing, int positionMs, int durationMs);
    // Returns (creating once, then reusing/updating) the MessageRowActions
    // for this eventId -- see the class comment above for why this is the
    // reply mechanism now. const because toDisplayItem() (which calls this)
    // is const; m_rowActions is mutable to allow the lazy cache.
    QObject* rowActionsFor(const QString &eventId, const QString &senderId, const QString &senderShort, const QString &bodyPreview, const QString &msgtype, bool isOutgoing) const;
    void setSending(bool sending);
    void setLoadingHistory(bool loading);
    void setHistoryExhausted(bool exhausted);
    void sendTypingState(bool typing);

    MatrixApi *m_api;
    TimelineStore *m_store;
    MediaManager *m_media;
    KeyBackupManager *m_keyBackup;
    OlmCryptoManager *m_olmCrypto;
    bb::cascades::ArrayDataModel *m_model;
    QString m_roomId;
    QString m_typingText;
    bool m_sending;
    bool m_loadingHistory;
    bool m_historyExhausted;
    bool m_typingActiveSent;
    QTimer *m_typingStopTimer;
    struct PendingUpload {
        QString roomId;
        QString msgtype;  // "m.image" or "m.audio"
        int durationMs;   // only meaningful for m.audio
    };
    QHash<QString, PendingUpload> m_pendingUploads; // localFilePath -> what to send once it's uploaded
    QHash<QNetworkReply*, QString> m_roomMembersReplyRoom;
    QHash<QString, QString> m_roomMemberNames;   // userId -> display_name, for m_roomId only
    QHash<QString, QString> m_roomMemberAvatars; // userId -> avatar_url mxc, for m_roomId only
    bool m_roomMembersKnown; // true once the /joined_members fetch for m_roomId has landed
    bool m_roomIsGroupChat;  // accurate once m_roomMembersKnown; a heuristic (per-event isGroupChat) until then
    QString m_playingAudioEventId;
    bool m_audioIsPlaying;
    int m_audioPositionMs;
    int m_audioDurationMs;
    int m_seekRequestMs;
    QVariantMap m_instagramVideoResult;
    QVariantMap m_replyTarget;
    QVariantMap m_editTarget;
    QString m_lastSendError;
    mutable QHash<QString, MessageRowActions*> m_rowActions; // eventId -> its row's action handler
};

#endif /* MESSAGELISTMODEL_HPP_ */
