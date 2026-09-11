#ifndef KEYBACKUPMANAGER_HPP_
#define KEYBACKUPMANAGER_HPP_

#include <QObject>
#include <QString>
#include <QByteArray>
#include <QHash>

class MatrixApi;
class QNetworkReply;

// Reads Matrix's server-side Secure Key Backup
// (m.megolm_backup.v1.curve25519-aes-sha2) using a user-supplied Recovery
// Key, so m.room.encrypted timeline events can be decrypted without
// implementing the full Olm device-verification / to-device key-sharing
// protocol (no /keys/upload, no SAS, no cross-signing).
//
// Modern accounts wrap the backup's actual Curve25519 private key inside
// Secure Secret Storage (SSSS): the Recovery Key only unlocks a 32-byte SSSS
// key, which in turn decrypts (HKDF-SHA256 + AES-256-CTR + HMAC-SHA256, the
// "m.secret_storage.v1.aes-hmac-sha2" algorithm) the account_data secret
// "m.megolm_backup.v1" to yield the real backup private key. unlock() fetches
// that secret directly and tries the Recovery Key against every key id
// present in it (some homeservers, e.g. Beeper, reject reads of the
// m.secret_storage.default_key / m.secret_storage.key.<id> metadata with
// M_FORBIDDEN even though m.megolm_backup.v1 itself is readable, so we never
// rely on that metadata to pick "the" key). Falls back to treating the
// Recovery Key as the private key directly when no m.megolm_backup.v1 secret
// exists at all (older, pre-SSSS accounts), then checks the resulting key
// against the backup's declared public key. Once unlocked, requestSession()
// lazily fetches and decrypts one Megolm session at a time from
// /room_keys/keys/{roomId}/{sessionId} as SyncEngine encounters ciphertext
// it can't yet decrypt.
//
// importFromProxy() is an independent, simpler path for homeservers (e.g.
// Beeper) that block the account_data reads above entirely: it pulls
// already-decrypted Megolm session keys from the TLS bridge proxy, which
// decrypts them locally from a standard Element "Export E2E room keys" file
// (a plain client-side export, no server interaction at all). Imported
// sessions land in the same m_sessions cache as the backup path, so
// hasSession()/decrypt() and SyncEngine's retroactive-update wiring work
// identically regardless of which path populated the cache.
class KeyBackupManager : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool unlocked READ isUnlocked NOTIFY unlockedChanged)
    Q_PROPERTY(bool busy READ isBusy NOTIFY busyChanged)
    Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged)
    Q_PROPERTY(QString importStatus READ importStatus NOTIFY importStatusChanged)

public:
    explicit KeyBackupManager(MatrixApi *api, QObject *parent = 0);
    virtual ~KeyBackupManager();

    bool isUnlocked() const;
    bool isBusy() const;
    QString lastError() const;
    QString importStatus() const;

    // Cache-only lookup: true if the Megolm session for (roomId, sessionId)
    // has already been fetched and decrypted this run.
    bool hasSession(const QString &roomId, const QString &sessionId) const;

    // Cache-only decrypt of one m.room.encrypted event's ciphertext.
    // Returns false (leaving *plaintextOut untouched) if the session isn't
    // cached yet or the ciphertext fails to decrypt. On success,
    // *plaintextOut holds the decrypted cleartext-event JSON.
    bool decrypt(const QString &roomId, const QString &sessionId, const QString &ciphertextBase64, QString *plaintextOut);

    // Imports a Megolm session shared live via an (already Olm-decrypted)
    // m.room_key to-device event -- called by OlmCryptoManager. Uses
    // olm_init_inbound_group_session (the "outbound_group_session_key"
    // format), NOT the same call as importFromProxy()'s Element-export path
    // (which uses the different "exported session" format). Emits
    // sessionReady() on success so SyncEngine can patch any placeholders
    // already showing for this session.
    bool importLiveSession(const QString &roomId, const QString &sessionId, const QString &sessionKeyB64);

public slots:
    void unlock(const QString &recoveryKey);
    void requestSession(const QString &roomId, const QString &sessionId);
    // Fetches the plaintext Megolm session list from the TLS bridge proxy's
    // /beport/megolm-sessions endpoint (see tools/tls-bridge-proxy.py) --
    // sessions decrypted locally from an Element "Export E2E room keys" file,
    // for homeservers that block the standard Secure Key Backup API.
    void importFromProxy();

signals:
    void unlockedChanged();
    void unlockFailed(const QString &error);
    void busyChanged();
    void lastErrorChanged();
    void importStatusChanged();
    void sessionReady(const QString &roomId, const QString &sessionId);
    void sessionFailed(const QString &roomId, const QString &sessionId);

private slots:
    void onMegolmSecretReplyFinished();
    void onVersionReplyFinished();
    void onSessionReplyFinished();
    void onProxyImportReplyFinished();

private:
    struct PendingSession {
        QString roomId;
        QString sessionId;
    };

    static bool parseRecoveryKey(const QString &input, QByteArray *privateKeyOut, QString *errorOut);
    static QString sessionKey(const QString &roomId, const QString &sessionId);
    QString accountDataPath(const QString &type) const;
    void fetchBackupVersion(const QByteArray &candidatePrivateKey);
    bool importSessionData(const QString &roomId, const QString &sessionId, const QByteArray &ephemeral, const QByteArray &mac, const QByteArray &ciphertext);
    bool importExportedSession(const QString &roomId, const QString &sessionId, const QString &sessionKeyB64);
    bool initInboundSessionFromKey(const QString &roomId, const QString &sessionId, const QString &sessionKeyB64);
    void setBusy(bool busy);
    void setLastError(const QString &error);
    void setImportStatus(const QString &status);
    void freePkDecryption();
    void freeSessions();

    MatrixApi *m_api;
    bool m_unlocked;
    bool m_busy;
    QString m_lastError;
    QString m_importStatus;
    QByteArray m_ssssRawKey;  // 32-byte SSSS key derived from the Recovery Key
    QString m_ssssKeyId;
    QString m_diagInfo; // which unlock path was taken, surfaced in error messages for debugging
    QString m_backupVersion;
    void *m_pkDecryptionMemory;
    QHash<QString, void*> m_sessions; // "roomId|sessionId" -> OlmInboundGroupSession memory
    QHash<QNetworkReply*, PendingSession> m_pendingByReply;
};

#endif /* KEYBACKUPMANAGER_HPP_ */
