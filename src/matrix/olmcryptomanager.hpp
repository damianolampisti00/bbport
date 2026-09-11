#ifndef OLMCRYPTOMANAGER_HPP_
#define OLMCRYPTOMANAGER_HPP_

#include <QObject>
#include <QString>
#include <QByteArray>
#include <QHash>
#include <QSet>
#include <QVariantMap>
#include <QVariantList>
#include <QStringList>
#include <QList>

class MatrixApi;
class KeyBackupManager;
class QNetworkReply;

// Implements the live device-to-device Olm/Megolm protocol: this device's
// identity (an Olm Account, uploaded via /keys/upload so other devices can
// find it), per-recipient-device 1:1 Olm sessions used to share Megolm room
// keys, and one outbound Megolm session per room used to encrypt outgoing
// messages. This is what lets BBport actually send messages other devices
// can decrypt, and receive newly-shared room keys going forward without
// relying on manual Element key exports (see KeyBackupManager, which still
// owns the inbound-session cache and decrypt logic -- this class only feeds
// it live-shared keys and handles the outbound side).
//
// The account and device_id are pickled to local disk (QDir::homePath() +
// "/bbport_olm/account.dat", mirroring MediaManager's cache directory
// convention) so the same device identity persists across app runs --
// otherwise every login would look like a brand new device to everyone
// else, unable to receive anything shared to the previous session's device.
class OlmCryptoManager : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString verificationStatus READ verificationStatus NOTIFY verificationStatusChanged)
    Q_PROPERTY(QString verificationSas READ verificationSas NOTIFY verificationSasChanged)
    Q_PROPERTY(QString verificationEmoji READ verificationEmoji NOTIFY verificationEmojiChanged)
    Q_PROPERTY(bool verificationAwaitingConfirm READ verificationAwaitingConfirm NOTIFY verificationAwaitingConfirmChanged)
    Q_PROPERTY(bool verificationIncomingPending READ verificationIncomingPending NOTIFY verificationIncomingChanged)
    Q_PROPERTY(QString verificationIncomingFrom READ verificationIncomingFrom NOTIFY verificationIncomingChanged)
    Q_PROPERTY(bool verificationActive READ verificationActive NOTIFY verificationStatusChanged)

public:
    OlmCryptoManager(MatrixApi *api, KeyBackupManager *keyBackup, QObject *parent = 0);
    virtual ~OlmCryptoManager();

    // Persisted device_id this identity uses. Read this before logging in
    // (MatrixApi::setPreferredDeviceId()) so the server reuses the same
    // device across app runs instead of minting a new one every time.
    QString deviceId() const;

    bool isRoomEncrypted(const QString &roomId) const;

    // Interactive SAS ("decimal" and "emoji") device verification, both directions:
    // BBport can initiate (startVerification(), sending .start directly to
    // one of the account's other devices -- skipping the newer .request/
    // .ready handshake, which some clients don't surface to their user) and
    // can also accept a verification another device starts against it
    // (handleVerificationEvent() -> acceptIncomingVerification()). Either
    // way, this is what lets bridges that gate relaying on device trust
    // (observed on Beeper's WhatsApp bridge) accept messages BBport sends.
    QString verificationStatus() const;
    QString verificationSas() const;   // "1234 - 5678 - 9012" once ready to compare
    QString verificationEmoji() const; // "🐧 Penguin, 🐎 Horse, ..." (7 emoji), same data as verificationSas
    bool verificationAwaitingConfirm() const;
    bool verificationIncomingPending() const; // another device wants to verify us
    QString verificationIncomingFrom() const; // its device_id, for display
    // True for the whole lifetime of a verification attempt (ours or
    // theirs), from .request/.start through to done/cancel. Meant to gate
    // "Verify device": starting a second attempt while one is stuck
    // (e.g. a self-initiated .start that Element never answers, since it
    // only seems to react to its own .request flow) would otherwise make
    // BBport silently ignore any real incoming .request until app restart,
    // since there's no timeout that clears a stuck attempt on its own.
    bool verificationActive() const;

public slots:
    // Uploads device identity keys (first call only) and tops up one-time
    // keys. Call once after login succeeds.
    void start();

    // Tracks which rooms are encrypted, fed from SyncEngine::roomUpdated.
    void onRoomUpdated(const QString &roomId, const QVariantMap &summary);

    // Processes one raw to-device event from /sync's "to_device.events".
    void handleToDeviceEvent(const QVariantMap &event);

    // Sends m.room_key_request (action: "request") to every one of this
    // account's OTHER devices (wildcard "*" device id) asking whoever
    // already has this specific Megolm session to forward it back via
    // m.forwarded_room_key. Recovers from the case where the device that
    // originally shared the key either never reached us (a stale/desynced
    // 1:1 Olm session with the sender silently drops the m.room_key --
    // olmDecryptFrom() has no way to recover that itself once the sender
    // insists on reusing that same broken session) or shared it before this
    // device existed. Called by SyncEngine the first time it sees a
    // ciphertext for a session it doesn't have.
    void requestRoomKey(const QString &roomId, const QString &sessionId, const QString &senderKey);

    // Encrypts `content` for roomId and sends it as a real m.room.encrypted
    // event, sharing the room's Megolm session key to any devices that don't
    // have it yet first. eventType is the type wrapped INSIDE the Megolm
    // payload (what the recipient decrypts back into "type") -- defaults to
    // m.room.message for plain/reply/edit sends, but m.reaction sends the
    // same way just needs this set to "m.reaction" instead. Asynchronous;
    // emits sendSucceeded/sendFailed for this txnId when done.
    void encryptAndSend(const QString &roomId, const QVariantMap &content, const QString &txnId, const QString &eventType = "m.room.message");

    // Sends m.key.verification.request to all of this account's other
    // devices. Whichever one accepts drives the rest of the exchange.
    void startVerification();
    // Call once the user has visually compared verificationSas() against
    // what the other device shows and confirms they match.
    void confirmVerification();
    // Call if the user says the numbers DON'T match, or to abort.
    void cancelVerification();
    // Call in response to verificationIncomingPending() to accept a
    // verification another device started against us.
    void acceptIncomingVerification();

signals:
    void sendSucceeded(const QString &txnId);
    void sendFailed(const QString &txnId, const QString &error);
    void verificationStatusChanged();
    void verificationSasChanged();
    void verificationEmojiChanged();
    void verificationAwaitingConfirmChanged();
    void verificationIncomingChanged();

private slots:
    void onKeysUploadReplyFinished();
    void onJoinedMembersReplyFinished();
    void onKeysQueryReplyFinished();
    void onKeysClaimReplyFinished();
    void onSendToDeviceReplyFinished();
    void onEncryptedSendReplyFinished();
    void onVerificationKeysQueryReplyFinished();

private:
    struct PendingSend {
        QVariantMap content;
        QString txnId;
        QString eventType;
    };
    struct RoomCrypto {
        RoomCrypto() : outboundSession(0), membersKnown(false), devicesKnown(false), queryInFlight(false) {}
        void *outboundSession; // OlmOutboundGroupSession*, 0 if not yet created
        QString sessionId;
        QSet<QString> sharedTo; // "userId|deviceId" already given this session's key
        QStringList memberIds;
        bool membersKnown;
        bool devicesKnown;
        bool queryInFlight;
        QList<PendingSend> pending;
        QHash<QString, QHash<QString, QVariantMap> > devices; // userId -> deviceId -> {curve25519,ed25519}
    };
    struct VerificationState {
        VerificationState() : sas(0), active(false), weStarted(false), incomingPending(false) {}
        QString transactionId;
        QString theirUserId;
        QString theirDeviceId;
        QVariantMap startContent; // the .start content, whichever side sent it
        QString theirCommitment;
        void *sas; // OlmSAS*
        QString ourPubkey;
        QString theirPubkey;
        bool active;
        bool weStarted;      // true: we sent .start. false: we're accepting theirs.
        bool incomingPending; // true: got their .start, waiting on our user to accept/reject
    };

    void loadOrCreateAccount();
    void persistAccount();
    static QByteArray accountPickleKey();
    void generateAndUploadOneTimeKeys();

    RoomCrypto &roomState(const QString &roomId);
    void ensureRoomReady(const QString &roomId);
    void shareKeyAndFlush(const QString &roomId);
    void sendRoomKeyToDevices(const QString &roomId, const QStringList &deviceTags);
    void flushPendingSends(const QString &roomId);
    void *ensureOutboundSession(const QString &roomId, RoomCrypto &state);
    void failPending(const QString &roomId, const QString &error);
    static void mergeDeviceKeysResponse(const QVariantMap &parsed, QHash<QString, QHash<QString, QVariantMap> > &target);

    void *olmSessionFor(const QString &theirIdentityKey) const;
    void storeOlmSession(const QString &theirIdentityKey, void *session);
    QVariantMap buildOlmPlaintext(const QString &type, const QVariantMap &content, const QString &recipientUserId, const QString &recipientEd25519Key) const;
    bool olmEncryptFor(const QString &theirIdentityKey, const QVariantMap &plaintextObj, int *outType, QString *outCiphertext);
    bool olmDecryptFrom(const QString &senderIdentityKey, int messageType, const QString &ciphertext, QString *outPlaintext);

    void handleVerificationEvent(const QVariantMap &event);
    void sendVerificationRequest();
    void sendVerificationReady();
    void sendVerificationStart();
    void sendVerificationAccept();
    void ensureTheirKeysThenAccept();
    void sendVerificationKey();
    void sendVerificationMac();
    void sendVerificationDone();
    void sendVerificationCancel(const QString &reason, const QString &code);
    void freeVerificationSas();
    QString sasInfo() const;
    QString macInfoForOurKey() const;
    QString macInfoForTheirKey() const;
    void setVerificationStatus(const QString &status);
    void setVerificationSas(const QString &sas);
    void setVerificationEmoji(const QString &emoji);
    void setVerificationAwaitingConfirm(bool awaiting);
    void setVerificationIncoming(bool pending, const QString &fromDevice);
    void sendToOneDevice(const QString &eventType, const QString &userId, const QString &deviceId, const QVariantMap &content);

    MatrixApi *m_api;
    KeyBackupManager *m_keyBackup;
    void *m_account; // OlmAccount*
    QString m_deviceId;
    QString m_identityKey;   // our curve25519 identity key, base64 (cached at load time)
    QString m_fingerprintKey; // our ed25519 identity key, base64 (cached at load time)
    QHash<QString, bool> m_encryptedRooms;
    QHash<QString, RoomCrypto> m_rooms;
    QHash<QString, void*> m_olmSessions; // their curve25519 identity key -> OlmSession*
    QHash<QString, QHash<QString, QVariantMap> > m_deviceKeys; // userId -> deviceId -> {curve25519,ed25519}, shared cache

    VerificationState m_verification;
    QString m_verificationStatus;
    QString m_verificationSas;
    QString m_verificationEmoji;
    bool m_verificationAwaitingConfirm;
    QString m_verificationIncomingFrom;
    QSet<QString> m_verifiedDevices; // "userId|deviceId"

    QHash<QNetworkReply*, QString> m_joinedMembersRoom;
    QHash<QNetworkReply*, QString> m_keysQueryRoom;
    QHash<QNetworkReply*, QString> m_keysClaimRoom;
    QHash<QNetworkReply*, QString> m_sendToDeviceRoom;
    QHash<QNetworkReply*, QStringList> m_sendToDeviceShared;
    QHash<QNetworkReply*, QString> m_encryptedSendTxn;
    QHash<QNetworkReply*, QString> m_encryptedSendRoom;
};

#endif /* OLMCRYPTOMANAGER_HPP_ */
