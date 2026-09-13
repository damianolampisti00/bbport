#include "keybackupmanager.hpp"
#include "matrixapi.hpp"
#include "base58.hpp"

#include <bb/data/JsonDataAccess>

#include <olm/pk.h>
#include <olm/inbound_group_session.h>
#include <olm/olm.h>
#include <olm/crypto.h>
#include <olm/base64.h>

extern "C" {
#include <crypto-algorithms/aes.h>
}

#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>
#include <QStringList>
#include <QFile>
#include <QDebug>
#include <cstdlib>
#include <cstring>

using namespace bb::data;

// Temporary diagnostic logging (SSSS unwrap bring-up). Remove once resolved.
static void appendSsssLog(const QString &line)
{
    QFile f("/accounts/1000/shared/misc/bbport_ssss_log.txt");
    if (f.open(QIODevice::Append)) {
        f.write((line + "\n").toUtf8());
        f.close();
    }
}

// Matrix uses unpadded base64 throughout; Qt's QByteArray::fromBase64() is
// not reliably tolerant of missing '=' padding, so decode with libolm's own
// (unpadded-native) base64 implementation instead, for every base64 field
// that doesn't already go through olm_pk_decrypt()/olm_group_decrypt()
// (which do their own internal base64 decoding).
static QByteArray olmBase64Decode(const QByteArray &input)
{
    size_t rawLen = _olm_decode_base64_length(input.size());
    if (rawLen == (size_t)-1) return QByteArray();
    QByteArray out(int(rawLen), '\0');
    _olm_decode_base64((const uint8_t*)input.constData(), input.size(), (uint8_t*)out.data());
    return out;
}

// Unwraps the "m.secret_storage.v1.aes-hmac-sha2" SSSS encryption scheme:
// AES-256-CTR with a key/HMAC-key pair derived from the 32-byte SSSS key via
// HKDF-SHA256 (salt = 32 zero bytes, info = the secret's event type name).
static void hkdfSha256(const QByteArray &ikm, const QByteArray &info, QByteArray *out64)
{
    QByteArray salt(32, '\0');
    out64->fill('\0', 64);
    // The vendored third_party/olm/include/olm/crypto.h declares this as
    // (input, info, salt, output), but the actual prebuilt libolm 3.2.16
    // .a we link against implements the newer upstream order (input, salt,
    // info, output) -- header/binary version mismatch. Verified empirically
    // against a real Beeper SSSS secret: with the header's declared order
    // the derived AES/HMAC keys never matched the server's HMAC for a
    // Recovery Key confirmed correct (independently replicated with a
    // standard RFC5869 HKDF implementation); swapping salt/info here
    // reproduces the actual library's output exactly.
    _olm_crypto_hkdf_sha256(
            (const uint8_t*)ikm.constData(), ikm.size(),
            (const uint8_t*)salt.constData(), salt.size(),
            (const uint8_t*)info.constData(), info.size(),
            (uint8_t*)out64->data(), out64->size());
}

// Verifies the HMAC-SHA256 over ciphertext and, if it matches, AES-256-CTR
// decrypts it in place. Returns false (leaving *plaintextOut untouched) on a
// MAC mismatch, which for this caller means "wrong key".
static bool ssssDecrypt(const QByteArray &aesKey, const QByteArray &hmacKey, const QByteArray &iv, const QByteArray &ciphertext, const QByteArray &expectedMac, QByteArray *plaintextOut)
{
    if (iv.size() != 16 || aesKey.size() != 32 || hmacKey.size() != 32) return false;

    unsigned char computedMac[32];
    _olm_crypto_hmac_sha256(
            (const uint8_t*)hmacKey.constData(), hmacKey.size(),
            (const uint8_t*)ciphertext.constData(), ciphertext.size(),
            computedMac);
    int compareLen = qMin(32, expectedMac.size());
    if (compareLen <= 0 || std::memcmp(computedMac, expectedMac.constData(), compareLen) != 0) return false;

    WORD keySchedule[60];
    aes_key_setup((const BYTE*)aesKey.constData(), keySchedule, 256);
    plaintextOut->resize(ciphertext.size());
    aes_decrypt_ctr((const BYTE*)ciphertext.constData(), ciphertext.size(),
            (BYTE*)plaintextOut->data(), keySchedule, 256, (const BYTE*)iv.constData());
    return true;
}

// Matrix Recovery Key layout: 0x8B 0x01 <32-byte private key> <parity byte>,
// base58-encoded (spec: "Recovery Key representation").
static const unsigned char kRecoveryKeyPrefix0 = 0x8B;
static const unsigned char kRecoveryKeyPrefix1 = 0x01;
static const int kRecoveryKeyRawLength = 35; // 2 prefix + 32 key + 1 parity

KeyBackupManager::KeyBackupManager(MatrixApi *api, QObject *parent) :
        QObject(parent),
        m_api(api),
        m_unlocked(false),
        m_busy(false),
        m_pkDecryptionMemory(0)
{
}

KeyBackupManager::~KeyBackupManager()
{
    freePkDecryption();
    freeSessions();
}

bool KeyBackupManager::isUnlocked() const { return m_unlocked; }
bool KeyBackupManager::isBusy() const { return m_busy; }
QString KeyBackupManager::lastError() const { return m_lastError; }

void KeyBackupManager::setBusy(bool busy)
{
    if (m_busy == busy) return;
    m_busy = busy;
    emit busyChanged();
}

void KeyBackupManager::setLastError(const QString &error)
{
    // General runtime instrumentation (see conversation): every unlock()
    // failure path already builds a specific, detailed error string here,
    // but nothing in main.qml ever displays lastError or reacts to
    // unlockFailed -- a wrong/mismatched Recovery Key (or any other unlock
    // failure) previously had zero visible feedback, silently leaving key
    // backup permanently locked for the whole session with no clue why.
    if (!error.isEmpty()) {
        qDebug() << "[BBport:keybackup]" << error;
    }
    m_lastError = error;
    emit lastErrorChanged();
}

void KeyBackupManager::freePkDecryption()
{
    if (m_pkDecryptionMemory) {
        olm_clear_pk_decryption(static_cast<OlmPkDecryption*>(m_pkDecryptionMemory));
        std::free(m_pkDecryptionMemory);
        m_pkDecryptionMemory = 0;
    }
}

void KeyBackupManager::freeSessions()
{
    QHashIterator<QString, void*> it(m_sessions);
    while (it.hasNext()) {
        it.next();
        olm_clear_inbound_group_session(static_cast<OlmInboundGroupSession*>(it.value()));
        std::free(it.value());
    }
    m_sessions.clear();
}

QString KeyBackupManager::sessionKey(const QString &roomId, const QString &sessionId)
{
    return roomId + "|" + sessionId;
}

bool KeyBackupManager::parseRecoveryKey(const QString &input, QByteArray *privateKeyOut, QString *errorOut)
{
    QByteArray raw;
    if (!Base58::decode(input, &raw)) {
        if (errorOut) *errorOut = "Invalid Recovery Key (unrecognized characters).";
        return false;
    }
    if (raw.size() != kRecoveryKeyRawLength) {
        // Includes the actual decoded byte count (not just "wrong") since
        // this is otherwise impossible to diagnose remotely -- e.g. a
        // clipboard paste that grabbed a few extra characters around the
        // key (a label like "Recovery Key: ...") would still decode as
        // valid base58 (most letters/digits ARE valid base58 digits) but
        // produce a too-long result, distinguishable from a simple typo/
        // missed-character case only by seeing the actual count.
        if (errorOut) *errorOut = QString("Invalid Recovery Key (unexpected length: got %1 bytes, expected %2).")
                .arg(raw.size()).arg(kRecoveryKeyRawLength);
        return false;
    }
    unsigned char parity = 0;
    for (int i = 0; i < raw.size() - 1; ++i) parity ^= (unsigned char)raw.at(i);
    if (parity != (unsigned char)raw.at(raw.size() - 1)) {
        if (errorOut) *errorOut = "Invalid Recovery Key (parity check failed).";
        return false;
    }
    if ((unsigned char)raw.at(0) != kRecoveryKeyPrefix0 || (unsigned char)raw.at(1) != kRecoveryKeyPrefix1) {
        if (errorOut) *errorOut = "Invalid Recovery Key (unexpected prefix).";
        return false;
    }
    *privateKeyOut = raw.mid(2, 32);
    return true;
}

QString KeyBackupManager::accountDataPath(const QString &type) const
{
    return QString("/user/%1/account_data/%2")
            .arg(QString(QUrl::toPercentEncoding(m_api->userId())))
            .arg(QString(QUrl::toPercentEncoding(type)));
}

void KeyBackupManager::unlock(const QString &recoveryKey)
{
    QString parseError;
    QByteArray rawKey;
    if (!parseRecoveryKey(recoveryKey, &rawKey, &parseError)) {
        setLastError(parseError);
        emit unlockFailed(parseError);
        return;
    }

    m_ssssRawKey = rawKey;
    setBusy(true);
    setLastError(QString());

    // Go straight for the wrapped secret rather than looking up
    // m.secret_storage.default_key / m.secret_storage.key.<id> first: some
    // homeservers (observed on Beeper) return M_FORBIDDEN for those SSSS
    // metadata types even though m.megolm_backup.v1 itself is readable. We
    // don't strictly need to know which key is "default" -- we just try the
    // Recovery Key against every entry present in "encrypted" and keep
    // whichever one its HMAC actually verifies against.
    QNetworkReply *reply = m_api->apiGet(accountDataPath("m.megolm_backup.v1"));
    connect(reply, SIGNAL(finished()), this, SLOT(onMegolmSecretReplyFinished()));
}

void KeyBackupManager::onMegolmSecretReplyFinished()
{
    QNetworkReply *reply = qobject_cast<QNetworkReply*>(sender());
    if (!reply) return;

    QByteArray rawBody = reply->readAll();
    {
        // Temporary diagnostic: dump the raw account_data body (iv/ciphertext/
        // mac skeleton, not message content) so the SSSS unwrap can be
        // reproduced/debugged offline. Remove once resolved.
        QFile logFile("/accounts/1000/shared/misc/bbport_megolm_secret.txt");
        if (logFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            logFile.write(rawBody);
            logFile.close();
        }
    }

    bool ok = false;
    QVariant parsed;
    {
        JsonDataAccess jda;
        parsed = jda.loadFromBuffer(rawBody);
        ok = !jda.hasError();
    }
    reply->deleteLater();

    QVariantMap encryptedByKeyId = parsed.toMap().value("encrypted").toMap();
    if (!ok || encryptedByKeyId.isEmpty()) {
        // No m.megolm_backup.v1 secret at all (legacy accounts use the
        // Recovery Key directly as the backup's Curve25519 private key).
        m_diagInfo = QString("legacy (m.megolm_backup.v1 assente), ok=%1 campi=[%2] errcode=%3")
                .arg(ok ? "1" : "0")
                .arg(QStringList(parsed.toMap().keys()).join(","))
                .arg(parsed.toMap().value("errcode").toString());
        fetchBackupVersion(m_ssssRawKey);
        return;
    }

    QStringList keyIds = encryptedByKeyId.keys();
    for (int i = 0; i < keyIds.size(); ++i) {
        const QString &keyId = keyIds.at(i);
        QVariantMap enc = encryptedByKeyId.value(keyId).toMap();
        // Standard, padded base64 as sent over the wire in the SSSS JSON
        // (e.g. "THrzkPYpzr0q2W7HJbiLqA==") -- NOT libolm's own unpadded
        // base64 variant that olmBase64Decode() is for. Using the wrong
        // decoder here silently produced wrong bytes and made every HMAC
        // check fail, even with a correct Recovery Key.
        QByteArray iv = QByteArray::fromBase64(enc.value("iv").toString().toUtf8());
        QByteArray ciphertext = QByteArray::fromBase64(enc.value("ciphertext").toString().toUtf8());
        QByteArray mac = QByteArray::fromBase64(enc.value("mac").toString().toUtf8());
        if (iv.isEmpty() || ciphertext.isEmpty() || mac.isEmpty()) continue;

        QByteArray derived;
        hkdfSha256(m_ssssRawKey, QByteArray("m.megolm_backup.v1"), &derived);
        QByteArray aesKey = derived.left(32);
        QByteArray hmacKey = derived.mid(32, 32);

        unsigned char computedMac[32];
        _olm_crypto_hmac_sha256((const uint8_t*)hmacKey.constData(), hmacKey.size(),
                (const uint8_t*)ciphertext.constData(), ciphertext.size(), computedMac);
        appendSsssLog(QString("ssssRawKey=%1 keyId=%2 iv.size=%3 ciphertext.size=%4 mac.size=%5")
                          .arg(QString(m_ssssRawKey.toHex())).arg(keyId)
                          .arg(iv.size()).arg(ciphertext.size()).arg(mac.size()));
        appendSsssLog(QString("aesKey=%1 hmacKey=%2").arg(QString(aesKey.toHex())).arg(QString(hmacKey.toHex())));
        appendSsssLog(QString("computedMac=%1 expectedMac=%2")
                          .arg(QString(QByteArray((const char*)computedMac, 32).toHex()))
                          .arg(QString(mac.toHex())));

        QByteArray secretPlaintext;
        if (!ssssDecrypt(aesKey, hmacKey, iv, ciphertext, mac, &secretPlaintext)) continue;

        QByteArray backupPrivateKey = olmBase64Decode(secretPlaintext);
        if (backupPrivateKey.size() != 32) continue;

        m_ssssKeyId = keyId;
        m_diagInfo = QString("ssss key_id=%1 (of %2 keys)").arg(keyId).arg(keyIds.size());
        fetchBackupVersion(backupPrivateKey);
        return;
    }

    setBusy(false);
    QString err = QString("Wrong Recovery Key: doesn't decrypt any of the %1 SSSS keys found.").arg(keyIds.size());
    setLastError(err);
    emit unlockFailed(err);
}

void KeyBackupManager::fetchBackupVersion(const QByteArray &candidatePrivateKey)
{
    void *mem = std::malloc(olm_pk_decryption_size());
    OlmPkDecryption *pk = olm_pk_decryption(mem);

    QByteArray pubkey(int(olm_pk_key_length()), '\0');
    size_t res = olm_pk_key_from_private(pk, pubkey.data(), pubkey.size(), candidatePrivateKey.constData(), candidatePrivateKey.size());
    if (res == olm_error()) {
        QString err = QString("Invalid Recovery Key: %1").arg(olm_pk_decryption_last_error(pk));
        olm_clear_pk_decryption(pk);
        std::free(mem);
        setBusy(false);
        setLastError(err);
        emit unlockFailed(err);
        return;
    }

    freePkDecryption();
    m_pkDecryptionMemory = mem;

    QNetworkReply *reply = m_api->apiGet("/room_keys/version");
    reply->setProperty("bbport_pending_pubkey", pubkey);
    connect(reply, SIGNAL(finished()), this, SLOT(onVersionReplyFinished()));
}

void KeyBackupManager::onVersionReplyFinished()
{
    QNetworkReply *reply = qobject_cast<QNetworkReply*>(sender());
    setBusy(false);
    if (!reply) return;

    bool ok = false;
    QVariant parsed = MatrixApi::parseJson(reply, &ok);
    QByteArray expectedPubkey = reply->property("bbport_pending_pubkey").toByteArray();
    reply->deleteLater();

    QVariantMap map = parsed.toMap();
    if (!ok || !map.contains("version")) {
        QString err = "Couldn't read the key backup from the server.";
        setLastError(err);
        emit unlockFailed(err);
        return;
    }

    QString algorithm = map.value("algorithm").toString();
    QString serverPubkey = map.value("auth_data").toMap().value("public_key").toString();
    if (algorithm != "m.megolm_backup.v1.curve25519-aes-sha2") {
        QString err = QString("Unsupported backup algorithm: %1").arg(algorithm);
        setLastError(err);
        emit unlockFailed(err);
        return;
    }
    if (serverPubkey.toUtf8() != expectedPubkey) {
        QString err = QString("Recovery Key doesn't match the backup (version %1). Expected: %2... Computed: %3... [%4]")
                .arg(map.value("version").toString())
                .arg(serverPubkey.left(8))
                .arg(QString::fromUtf8(expectedPubkey.left(8)))
                .arg(m_diagInfo);
        freePkDecryption();
        setLastError(err);
        emit unlockFailed(err);
        return;
    }

    m_backupVersion = map.value("version").toString();
    // Deliberately NOT freeSessions() here: unlock() races against live
    // key delivery (OlmCryptoManager's own m.room_key to-device imports via
    // importLiveSession(), and outgoing-session creation) since both start
    // right alongside SyncEngine's first /sync at login. unlock() itself
    // needs two sequential network round-trips, so a session imported
    // moments before this line lands would otherwise be destroyed right as
    // it arrives -- that message then wrongly shows the "couldn't decrypt"
    // placeholder despite the key having been available the whole time. Both import
    // paths already free any stale entry for the same key before
    // overwriting it, so there's nothing here that actually needs a clean
    // slate.
    m_unlocked = true;
    qDebug() << "[BBport:keybackup] unlocked, backup version=" << m_backupVersion;
    emit unlockedChanged();
}

bool KeyBackupManager::hasSession(const QString &roomId, const QString &sessionId) const
{
    return m_sessions.contains(sessionKey(roomId, sessionId));
}

bool KeyBackupManager::importSessionData(const QString &roomId, const QString &sessionId, const QByteArray &ephemeral, const QByteArray &mac, const QByteArray &ciphertextIn)
{
    OlmPkDecryption *pk = static_cast<OlmPkDecryption*>(m_pkDecryptionMemory);
    if (!pk) return false;

    QByteArray ciphertext = ciphertextIn; // pk_decrypt destroys this buffer in place
    QByteArray plaintext(ciphertext.size(), '\0');

    size_t plainLen = olm_pk_decrypt(pk,
            ephemeral.constData(), ephemeral.size(),
            mac.constData(), mac.size(),
            ciphertext.data(), ciphertext.size(),
            plaintext.data(), plaintext.size());
    if (plainLen == olm_error()) {
        appendSsssLog(QString("importSessionData: olm_pk_decrypt failed: %1 (ephLen=%2 macLen=%3 ctLen=%4)")
                          .arg(olm_pk_decryption_last_error(pk)).arg(ephemeral.size()).arg(mac.size()).arg(ciphertext.size()));
        return false;
    }
    plaintext.truncate(int(plainLen));

    JsonDataAccess jda;
    QVariant sessionJson = jda.loadFromBuffer(plaintext);
    if (jda.hasError()) {
        appendSsssLog(QString("importSessionData: JSON parse failed, plaintext=%1").arg(QString::fromUtf8(plaintext.left(200))));
        return false;
    }
    QString sessionKeyB64 = sessionJson.toMap().value("session_key").toString();
    if (sessionKeyB64.isEmpty()) {
        appendSsssLog(QString("importSessionData: no session_key field, plaintext=%1").arg(QString::fromUtf8(plaintext.left(200))));
        return false;
    }

    // The server-side key backup's session_key is in the "exported session"
    // format (same as Element's manual key-export files) -- NOT the format
    // olm_init_inbound_group_session()/initInboundSessionFromKey() expects,
    // which is only for live m.room_key to-device shares (importLiveSession).
    // Using the wrong one made olm_pk_decrypt succeed but every single
    // session import fail afterwards.
    bool ok = importExportedSession(roomId, sessionId, sessionKeyB64);
    appendSsssLog(QString("importSessionData: importExportedSession=%1").arg(ok ? "1" : "0"));
    return ok;
}

bool KeyBackupManager::initInboundSessionFromKey(const QString &roomId, const QString &sessionId, const QString &sessionKeyB64)
{
    void *mem = std::malloc(olm_inbound_group_session_size());
    OlmInboundGroupSession *session = olm_inbound_group_session(mem);
    QByteArray sessionKeyBytes = sessionKeyB64.toUtf8(); // olm_init_inbound_group_session overwrites this buffer
    size_t initRes = olm_init_inbound_group_session(session,
            (const uint8_t*)sessionKeyBytes.data(), sessionKeyBytes.size());
    if (initRes == olm_error()) {
        olm_clear_inbound_group_session(session);
        std::free(mem);
        return false;
    }

    QString key = sessionKey(roomId, sessionId);
    if (void *old = m_sessions.value(key)) {
        olm_clear_inbound_group_session(static_cast<OlmInboundGroupSession*>(old));
        std::free(old);
    }
    m_sessions[key] = mem;
    return true;
}

bool KeyBackupManager::importLiveSession(const QString &roomId, const QString &sessionId, const QString &sessionKeyB64)
{
    if (!initInboundSessionFromKey(roomId, sessionId, sessionKeyB64)) return false;
    emit sessionReady(roomId, sessionId);
    return true;
}

bool KeyBackupManager::importForwardedSession(const QString &roomId, const QString &sessionId, const QString &sessionKeyB64)
{
    if (!importExportedSession(roomId, sessionId, sessionKeyB64)) return false;
    emit sessionReady(roomId, sessionId);
    return true;
}

bool KeyBackupManager::decrypt(const QString &roomId, const QString &sessionId, const QString &ciphertextBase64, QString *plaintextOut)
{
    void *mem = m_sessions.value(sessionKey(roomId, sessionId));
    if (!mem) return false;
    OlmInboundGroupSession *session = static_cast<OlmInboundGroupSession*>(mem);

    QByteArray message = ciphertextBase64.toUtf8(); // olm_group_decrypt destroys this buffer
    QByteArray plaintext(message.size(), '\0');
    uint32_t messageIndex = 0;

    size_t plainLen = olm_group_decrypt(session,
            (uint8_t*)message.data(), message.size(),
            (uint8_t*)plaintext.data(), plaintext.size(),
            &messageIndex);
    if (plainLen == olm_error()) return false;

    plaintext.truncate(int(plainLen));
    if (plaintextOut) *plaintextOut = QString::fromUtf8(plaintext);
    return true;
}

bool KeyBackupManager::importExportedSession(const QString &roomId, const QString &sessionId, const QString &sessionKeyB64)
{
    void *mem = std::malloc(olm_inbound_group_session_size());
    OlmInboundGroupSession *session = olm_inbound_group_session(mem);
    QByteArray sessionKeyBytes = sessionKeyB64.toUtf8(); // olm_import_inbound_group_session overwrites this buffer
    size_t res = olm_import_inbound_group_session(session,
            (uint8_t*)sessionKeyBytes.data(), sessionKeyBytes.size());
    if (res == olm_error()) {
        olm_clear_inbound_group_session(session);
        std::free(mem);
        return false;
    }

    QString key = sessionKey(roomId, sessionId);
    if (void *old = m_sessions.value(key)) {
        olm_clear_inbound_group_session(static_cast<OlmInboundGroupSession*>(old));
        std::free(old);
    }
    m_sessions[key] = mem;
    return true;
}

void KeyBackupManager::requestSession(const QString &roomId, const QString &sessionId)
{
    if (!m_unlocked || hasSession(roomId, sessionId)) return;

    QString path = QString("/room_keys/keys/%1/%2")
            .arg(QString(QUrl::toPercentEncoding(roomId)))
            .arg(QString(QUrl::toPercentEncoding(sessionId)));
    QVariantMap query;
    query["version"] = m_backupVersion;

    QNetworkReply *reply = m_api->apiGet(path, query);
    PendingSession pending;
    pending.roomId = roomId;
    pending.sessionId = sessionId;
    m_pendingByReply[reply] = pending;
    connect(reply, SIGNAL(finished()), this, SLOT(onSessionReplyFinished()));
}

void KeyBackupManager::onSessionReplyFinished()
{
    QNetworkReply *reply = qobject_cast<QNetworkReply*>(sender());
    if (!reply) return;
    if (!m_pendingByReply.contains(reply)) {
        reply->deleteLater();
        return;
    }
    PendingSession pending = m_pendingByReply.take(reply);

    bool ok = false;
    QVariant parsed = MatrixApi::parseJson(reply, &ok);
    reply->deleteLater();

    QVariantMap sessionData = parsed.toMap().value("session_data").toMap();
    QByteArray ephemeral = sessionData.value("ephemeral").toString().toUtf8();
    QByteArray mac = sessionData.value("mac").toString().toUtf8();
    QByteArray ciphertext = sessionData.value("ciphertext").toString().toUtf8();

    bool imported = ok && !ephemeral.isEmpty() && !mac.isEmpty() && !ciphertext.isEmpty()
            && importSessionData(pending.roomId, pending.sessionId, ephemeral, mac, ciphertext);
    appendSsssLog(QString("requestSession room=%1 session=%2 httpOk=%3 ephemeral=%4 mac=%5 ciphertext=%6 imported=%7")
                      .arg(pending.roomId).arg(pending.sessionId).arg(ok ? "1" : "0")
                      .arg(QString::fromUtf8(ephemeral)).arg(QString::fromUtf8(mac))
                      .arg(QString::fromUtf8(ciphertext)).arg(imported ? "1" : "0"));
    if (!imported) {
        emit sessionFailed(pending.roomId, pending.sessionId);
        return;
    }

    emit sessionReady(pending.roomId, pending.sessionId);
}
