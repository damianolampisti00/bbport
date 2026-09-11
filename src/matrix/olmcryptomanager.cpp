#include "olmcryptomanager.hpp"
#include "matrixapi.hpp"
#include "keybackupmanager.hpp"

#include <bb/data/JsonDataAccess>

#include <olm/olm.h>
#include <olm/sas.h>

#include <QNetworkReply>
#include <QDir>
#include <QFile>
#include <QUrl>
#include <QUuid>
#include <QDateTime>
#include <QMapIterator>
#include <QHashIterator>
#include <QMutex>
#include <QMutexLocker>
#include <cstdlib>
#include <stdio.h>

using namespace bb::data;

// Temporary diagnostic logging (E2EE key-share bring-up) -- writes straight
// to a file since qWarning()/slog2 wasn't showing up in testing. Remove once
// the "new messages still arrive undecryptable after a fresh device
// verification" issue is root-caused.
namespace {
QMutex g_olmLogMutex;
void appendOlmLog(const QString &line)
{
    QMutexLocker locker(&g_olmLogMutex);
    FILE *f = fopen("/accounts/1000/shared/misc/beport_olm_log.txt", "a");
    if (!f) return;
    QByteArray utf8 = (line + "\n").toUtf8();
    fwrite(utf8.constData(), 1, utf8.size(), f);
    fclose(f);
}
}

static QByteArray randomBytes(int len)
{
    QByteArray result;
    while (result.size() < len) result.append(QUuid::createUuid().toRfc4122());
    result.truncate(len);
    return result;
}

static QString generateDeviceId()
{
    QString hex = QUuid::createUuid().toString();
    hex.remove('{').remove('}').remove('-');
    return "BEPORT" + hex.left(8).toUpper();
}

// Matrix's JSON-signing procedure (https://spec.matrix.org/latest/appendices/#canonical-json)
// requires a canonical serialization: sorted object keys, no insignificant
// whitespace. QVariantMap is a QMap<QString,QVariant>, which already
// iterates keys in ascending order, so we only need to control whitespace
// and recurse manually rather than trust JsonDataAccess's own formatting
// for this specific, signature-critical step.
static void appendJsonString(QByteArray &out, const QString &s)
{
    out.append('"');
    QByteArray utf8 = s.toUtf8();
    for (int i = 0; i < utf8.size(); ++i) {
        unsigned char c = (unsigned char)utf8.at(i);
        switch (c) {
            case '"': out.append("\\\""); break;
            case '\\': out.append("\\\\"); break;
            case '\n': out.append("\\n"); break;
            case '\r': out.append("\\r"); break;
            case '\t': out.append("\\t"); break;
            default:
                if (c < 0x20) {
                    QString esc = QString("\\u%1").arg(c, 4, 16, QChar('0'));
                    out.append(esc.toLatin1());
                } else {
                    out.append((char)c);
                }
        }
    }
    out.append('"');
}

static void canonicalJsonAppend(QByteArray &out, const QVariant &v)
{
    if (v.type() == QVariant::Map) {
        QVariantMap m = v.toMap();
        out.append('{');
        bool first = true;
        QMapIterator<QString, QVariant> it(m);
        while (it.hasNext()) {
            it.next();
            if (!first) out.append(',');
            first = false;
            appendJsonString(out, it.key());
            out.append(':');
            canonicalJsonAppend(out, it.value());
        }
        out.append('}');
    } else if (v.type() == QVariant::List || v.type() == QVariant::StringList) {
        QVariantList l = v.toList();
        out.append('[');
        for (int i = 0; i < l.size(); ++i) {
            if (i) out.append(',');
            canonicalJsonAppend(out, l.at(i));
        }
        out.append(']');
    } else if (v.type() == QVariant::Bool) {
        out.append(v.toBool() ? "true" : "false");
    } else if (v.type() == QVariant::Int || v.type() == QVariant::LongLong
            || v.type() == QVariant::UInt || v.type() == QVariant::ULongLong) {
        out.append(QByteArray::number(v.toLongLong()));
    } else {
        appendJsonString(out, v.toString());
    }
}

static QByteArray canonicalJson(const QVariantMap &m)
{
    QByteArray out;
    canonicalJsonAppend(out, QVariant(m));
    return out;
}

// The fixed 64-entry SAS emoji table from the Matrix spec (Client-Server
// API, "SAS method: emoji"), in index order -- this exact glyph+order
// pairing is what every interoperable client (Element included) uses, so a
// wrong entry here wouldn't just look different, it would make Beport's
// emoji never match another client's for the same underlying secret. Names
// are the spec's canonical English ones; Element itself only localizes them
// for display; the glyph is what the user actually compares.
struct SasEmoji { const char *glyph; const char *name; };
static const SasEmoji kSasEmojiTable[64] = {
    { "\xF0\x9F\x90\xB6", "Dog" }, { "\xF0\x9F\x90\xB1", "Cat" }, { "\xF0\x9F\xA6\x81", "Lion" }, { "\xF0\x9F\x90\x8E", "Horse" },
    { "\xF0\x9F\xA6\x84", "Unicorn" }, { "\xF0\x9F\x90\xB7", "Pig" }, { "\xF0\x9F\x90\x98", "Elephant" }, { "\xF0\x9F\x90\xB0", "Rabbit" },
    { "\xF0\x9F\x90\xBC", "Panda" }, { "\xF0\x9F\x90\x93", "Rooster" }, { "\xF0\x9F\x90\xA7", "Penguin" }, { "\xF0\x9F\x90\xA2", "Turtle" },
    { "\xF0\x9F\x90\x9F", "Fish" }, { "\xF0\x9F\x90\x99", "Octopus" }, { "\xF0\x9F\xA6\x8B", "Butterfly" }, { "\xF0\x9F\x8C\xB7", "Flower" },
    { "\xF0\x9F\x8C\xB3", "Tree" }, { "\xF0\x9F\x8C\xB5", "Cactus" }, { "\xF0\x9F\x8D\x84", "Mushroom" }, { "\xF0\x9F\x8C\x8F", "Globe" },
    { "\xF0\x9F\x8C\x99", "Moon" }, { "\xE2\x98\x81\xEF\xB8\x8F", "Cloud" }, { "\xF0\x9F\x94\xA5", "Fire" }, { "\xF0\x9F\x8D\x8C", "Banana" },
    { "\xF0\x9F\x8D\x8E", "Apple" }, { "\xF0\x9F\x8D\x93", "Strawberry" }, { "\xF0\x9F\x8C\xBD", "Corn" }, { "\xF0\x9F\x8D\x95", "Pizza" },
    { "\xF0\x9F\x8E\x82", "Cake" }, { "\xE2\x9D\xA4\xEF\xB8\x8F", "Heart" }, { "\xF0\x9F\x98\x80", "Smiley" }, { "\xF0\x9F\xA4\x96", "Robot" },
    { "\xF0\x9F\x8E\xA9", "Hat" }, { "\xF0\x9F\x91\x93", "Glasses" }, { "\xF0\x9F\x94\xA7", "Spanner" }, { "\xF0\x9F\x8E\x85", "Santa" },
    { "\xF0\x9F\x91\x8D", "Thumbs Up" }, { "\xE2\x98\x82\xEF\xB8\x8F", "Umbrella" }, { "\xE2\x8C\x9B", "Hourglass" }, { "\xE2\x8F\xB0", "Clock" },
    { "\xF0\x9F\x8E\x81", "Gift" }, { "\xF0\x9F\x92\xA1", "Light Bulb" }, { "\xF0\x9F\x93\x95", "Book" }, { "\xE2\x9C\x8F\xEF\xB8\x8F", "Pencil" },
    { "\xF0\x9F\x93\x8E", "Paperclip" }, { "\xE2\x9C\x82\xEF\xB8\x8F", "Scissors" }, { "\xF0\x9F\x94\x92", "Lock" }, { "\xF0\x9F\x94\x91", "Key" },
    { "\xF0\x9F\x94\xA8", "Hammer" }, { "\xE2\x98\x8E\xEF\xB8\x8F", "Telephone" }, { "\xF0\x9F\x8F\x81", "Flag" }, { "\xF0\x9F\x9A\x82", "Train" },
    { "\xF0\x9F\x9A\xB2", "Bicycle" }, { "\xE2\x9C\x88\xEF\xB8\x8F", "Aeroplane" }, { "\xF0\x9F\x9A\x80", "Rocket" }, { "\xF0\x9F\x8F\x86", "Trophy" },
    { "\xE2\x9A\xBD", "Ball" }, { "\xF0\x9F\x8E\xB8", "Guitar" }, { "\xF0\x9F\x8E\xBA", "Trumpet" }, { "\xF0\x9F\x94\x94", "Bell" },
    { "\xE2\x9A\x93", "Anchor" }, { "\xF0\x9F\x8E\xA7", "Headphones" }, { "\xF0\x9F\x93\x81", "Folder" }, { "\xF0\x9F\x93\x8C", "Pin" },
};

// Splits the 42 most-significant bits of a 6-byte SAS output into seven
// 6-bit indices (0-63), per the Matrix spec's emoji method -- the standard
// bit layout used by every interoperable client, so this must match exactly
// or Beport's emoji would never agree with anyone else's for the same secret.
static QString emojiStringForSasBytes(const unsigned char *b)
{
    int idx[7];
    idx[0] = (b[0] & 0xFC) >> 2;
    idx[1] = ((b[0] & 0x3) << 4) | (b[1] >> 4);
    idx[2] = ((b[1] & 0xF) << 2) | (b[2] >> 6);
    idx[3] = b[2] & 0x3F;
    idx[4] = (b[3] & 0xFC) >> 2;
    idx[5] = ((b[3] & 0x3) << 4) | (b[4] >> 4);
    idx[6] = ((b[4] & 0xF) << 2) | (b[5] >> 6);

    QStringList parts;
    for (int i = 0; i < 7; ++i) {
        const SasEmoji &e = kSasEmojiTable[idx[i]];
        parts << QString("%1 %2").arg(QString::fromUtf8(e.glyph), QString::fromUtf8(e.name));
    }
    return parts.join(QString(", "));
}

OlmCryptoManager::OlmCryptoManager(MatrixApi *api, KeyBackupManager *keyBackup, QObject *parent) :
        QObject(parent),
        m_api(api),
        m_keyBackup(keyBackup),
        m_account(0),
        m_verificationAwaitingConfirm(false)
{
    loadOrCreateAccount();
}

OlmCryptoManager::~OlmCryptoManager()
{
    if (m_account) {
        olm_clear_account(static_cast<OlmAccount*>(m_account));
        std::free(m_account);
    }
    QHashIterator<QString, void*> sessIt(m_olmSessions);
    while (sessIt.hasNext()) {
        sessIt.next();
        olm_clear_session(static_cast<OlmSession*>(sessIt.value()));
        std::free(sessIt.value());
    }
    QHashIterator<QString, RoomCrypto> roomIt(m_rooms);
    while (roomIt.hasNext()) {
        roomIt.next();
        if (roomIt.value().outboundSession) {
            olm_clear_outbound_group_session(static_cast<OlmOutboundGroupSession*>(roomIt.value().outboundSession));
            std::free(roomIt.value().outboundSession);
        }
    }
    freeVerificationSas();
}

QString OlmCryptoManager::deviceId() const { return m_deviceId; }

bool OlmCryptoManager::isRoomEncrypted(const QString &roomId) const
{
    return m_encryptedRooms.value(roomId, false);
}

QString OlmCryptoManager::verificationStatus() const { return m_verificationStatus; }
QString OlmCryptoManager::verificationSas() const { return m_verificationSas; }
QString OlmCryptoManager::verificationEmoji() const { return m_verificationEmoji; }
bool OlmCryptoManager::verificationAwaitingConfirm() const { return m_verificationAwaitingConfirm; }
bool OlmCryptoManager::verificationIncomingPending() const { return m_verification.incomingPending; }
QString OlmCryptoManager::verificationIncomingFrom() const { return m_verificationIncomingFrom; }
bool OlmCryptoManager::verificationActive() const { return m_verification.active; }

void OlmCryptoManager::setVerificationStatus(const QString &status)
{
    m_verificationStatus = status;
    emit verificationStatusChanged();
}

void OlmCryptoManager::setVerificationSas(const QString &sas)
{
    m_verificationSas = sas;
    emit verificationSasChanged();
}

void OlmCryptoManager::setVerificationEmoji(const QString &emoji)
{
    m_verificationEmoji = emoji;
    emit verificationEmojiChanged();
}

void OlmCryptoManager::setVerificationAwaitingConfirm(bool awaiting)
{
    m_verificationAwaitingConfirm = awaiting;
    emit verificationAwaitingConfirmChanged();
}

void OlmCryptoManager::setVerificationIncoming(bool pending, const QString &fromDevice)
{
    m_verification.incomingPending = pending;
    m_verificationIncomingFrom = fromDevice;
    emit verificationIncomingChanged();
}

void OlmCryptoManager::freeVerificationSas()
{
    if (m_verification.sas) {
        olm_clear_sas(static_cast<OlmSAS*>(m_verification.sas));
        std::free(m_verification.sas);
        m_verification.sas = 0;
    }
}

void OlmCryptoManager::onRoomUpdated(const QString &roomId, const QVariantMap &summary)
{
    m_encryptedRooms[roomId] = summary.value("encrypted").toBool();
}

QByteArray OlmCryptoManager::accountPickleKey()
{
    // Local-device-only storage, never transmitted: this constant just
    // satisfies libolm's pickle API, which always encrypts. It is not a
    // secret in the network-facing sense -- protection here relies on the
    // OS/app sandbox around this file, the same trust model as any other
    // plaintext local app data.
    return QByteArray("beport-local-olm-pickle-key-v1");
}

void OlmCryptoManager::loadOrCreateAccount()
{
    QString dir = QDir::homePath() + "/beport_olm";
    QDir().mkpath(dir);
    QString path = dir + "/account.dat";

    bool loaded = false;
    QFile file(path);
    if (file.open(QIODevice::ReadOnly)) {
        QByteArray raw = file.readAll();
        file.close();
        JsonDataAccess jda;
        QVariant parsed = jda.loadFromBuffer(raw);
        if (!jda.hasError()) {
            QVariantMap map = parsed.toMap();
            QString deviceId = map.value("deviceId").toString();
            QByteArray pickle = map.value("pickle").toString().toUtf8();
            if (!deviceId.isEmpty() && !pickle.isEmpty()) {
                void *mem = std::malloc(olm_account_size());
                OlmAccount *account = olm_account(mem);
                QByteArray key = accountPickleKey();
                size_t res = olm_unpickle_account(account, key.constData(), key.size(), pickle.data(), pickle.size());
                if (res != olm_error()) {
                    m_account = mem;
                    m_deviceId = deviceId;
                    loaded = true;
                } else {
                    olm_clear_account(account);
                    std::free(mem);
                }
            }
        }
    }

    if (!loaded) {
        void *mem = std::malloc(olm_account_size());
        OlmAccount *account = olm_account(mem);
        size_t randLen = olm_create_account_random_length(account);
        QByteArray random = randomBytes(int(randLen));
        size_t res = olm_create_account(account, random.data(), random.size());
        if (res == olm_error()) {
            olm_clear_account(account);
            std::free(mem);
            return;
        }
        m_account = mem;
        m_deviceId = generateDeviceId();
    }

    OlmAccount *account = static_cast<OlmAccount*>(m_account);
    QByteArray idBuf(int(olm_account_identity_keys_length(account)), '\0');
    olm_account_identity_keys(account, idBuf.data(), idBuf.size());
    JsonDataAccess jda;
    QVariantMap idKeys = jda.loadFromBuffer(idBuf).toMap();
    m_identityKey = idKeys.value("curve25519").toString();
    m_fingerprintKey = idKeys.value("ed25519").toString();

    if (!loaded) persistAccount();
}

void OlmCryptoManager::persistAccount()
{
    if (!m_account) return;
    OlmAccount *account = static_cast<OlmAccount*>(m_account);
    QByteArray key = accountPickleKey();
    QByteArray pickle(int(olm_pickle_account_length(account)), '\0');
    size_t res = olm_pickle_account(account, key.constData(), key.size(), pickle.data(), pickle.size());
    if (res == olm_error()) return;
    pickle.truncate(int(res));

    QVariantMap map;
    map["deviceId"] = m_deviceId;
    map["pickle"] = QString::fromUtf8(pickle);

    JsonDataAccess jda;
    QByteArray out;
    jda.saveToBuffer(QVariant(map), &out);

    QString dir = QDir::homePath() + "/beport_olm";
    QDir().mkpath(dir);
    QFile file(dir + "/account.dat");
    if (file.open(QIODevice::WriteOnly)) {
        file.write(out);
        file.close();
    }
}

void OlmCryptoManager::start()
{
    generateAndUploadOneTimeKeys();
}

void OlmCryptoManager::generateAndUploadOneTimeKeys()
{
    if (!m_account) return;
    OlmAccount *account = static_cast<OlmAccount*>(m_account);
    const int kBatch = 20;

    size_t randLen = olm_account_generate_one_time_keys_random_length(account, kBatch);
    QByteArray random = randomBytes(int(randLen));
    olm_account_generate_one_time_keys(account, kBatch, random.data(), random.size());

    QByteArray otkBuf(int(olm_account_one_time_keys_length(account)), '\0');
    olm_account_one_time_keys(account, otkBuf.data(), otkBuf.size());

    JsonDataAccess jda;
    QVariantMap curveKeys = jda.loadFromBuffer(otkBuf).toMap().value("curve25519").toMap();

    QString userId = m_api->userId();
    QVariantMap uploadOtk;
    QMapIterator<QString, QVariant> it(curveKeys);
    while (it.hasNext()) {
        it.next();
        QVariantMap signedKey;
        signedKey["key"] = it.value().toString();
        QByteArray canonical = canonicalJson(signedKey);
        QByteArray sigBuf(int(olm_account_signature_length(account)), '\0');
        size_t sigRes = olm_account_sign(account, canonical.constData(), canonical.size(), sigBuf.data(), sigBuf.size());
        if (sigRes == olm_error()) continue;
        sigBuf.truncate(int(sigRes));
        QVariantMap sigMap;
        sigMap[QString("ed25519:%1").arg(m_deviceId)] = QString::fromUtf8(sigBuf);
        QVariantMap sigsByUser;
        sigsByUser[userId] = sigMap;
        signedKey["signatures"] = sigsByUser;
        uploadOtk[QString("signed_curve25519:%1").arg(it.key())] = signedKey;
    }

    // device_keys is included on every call, not just the first: uploading
    // it is idempotent (same device_id, same keys, servers just accept it
    // again) and cheap, and this makes us self-healing against exactly the
    // failure mode where an earlier run marked m_deviceKeysUploaded=true off
    // a 200 from /keys/upload without that proving OTHER clients could
    // actually validate our self-signature -- a bad signature (or any other
    // corruption) uploaded once would otherwise stay permanently invisible
    // to us, since the server never verifies it and we'd never retry. This
    // was observed live: Element showed our own device as "does not support
    // encryption" (no keys it could validate) despite /keys/upload always
    // replying 200.
    QVariantMap body;
    QVariantMap deviceKeys;
    deviceKeys["user_id"] = userId;
    deviceKeys["device_id"] = m_deviceId;
    QVariantList algos;
    algos << "m.olm.v1.curve25519-aes-sha2" << "m.megolm.v1.aes-sha2";
    deviceKeys["algorithms"] = algos;
    QVariantMap keys;
    keys[QString("curve25519:%1").arg(m_deviceId)] = m_identityKey;
    keys[QString("ed25519:%1").arg(m_deviceId)] = m_fingerprintKey;
    deviceKeys["keys"] = keys;

    QByteArray canonicalDevice = canonicalJson(deviceKeys);
    QByteArray sigBuf(int(olm_account_signature_length(account)), '\0');
    size_t sigRes = olm_account_sign(account, canonicalDevice.constData(), canonicalDevice.size(), sigBuf.data(), sigBuf.size());
    if (sigRes != olm_error()) {
        sigBuf.truncate(int(sigRes));
        QVariantMap sigMap;
        sigMap[QString("ed25519:%1").arg(m_deviceId)] = QString::fromUtf8(sigBuf);
        QVariantMap sigsByUser;
        sigsByUser[userId] = sigMap;
        deviceKeys["signatures"] = sigsByUser;
    }
    body["device_keys"] = deviceKeys;
    body["one_time_keys"] = uploadOtk;

    QNetworkReply *reply = m_api->apiPost("/keys/upload", body);
    connect(reply, SIGNAL(finished()), this, SLOT(onKeysUploadReplyFinished()));
}

void OlmCryptoManager::onKeysUploadReplyFinished()
{
    QNetworkReply *reply = qobject_cast<QNetworkReply*>(sender());
    if (!reply) return;
    bool ok = false;
    MatrixApi::parseJson(reply, &ok);
    reply->deleteLater();
    if (!ok || !m_account) return;

    olm_account_mark_keys_as_published(static_cast<OlmAccount*>(m_account));
    persistAccount();
}

void OlmCryptoManager::handleToDeviceEvent(const QVariantMap &event)
{
    if (!m_account) { appendOlmLog("handleToDeviceEvent: no account"); return; }

    QString eventType = event.value("type").toString();
    appendOlmLog(QString("to_device event type=%1").arg(eventType));
    if (eventType.startsWith("m.key.verification.")) {
        handleVerificationEvent(event);
        return;
    }
    if (eventType != "m.room.encrypted") return;

    QVariantMap content = event.value("content").toMap();
    if (content.value("algorithm").toString() != "m.olm.v1.curve25519-aes-sha2") {
        appendOlmLog(QString("  wrong algorithm: %1").arg(content.value("algorithm").toString()));
        return;
    }

    QString senderKey = content.value("sender_key").toString();
    QVariantMap ciphertextMap = content.value("ciphertext").toMap();
    if (!ciphertextMap.contains(m_identityKey)) {
        appendOlmLog(QString("  not addressed to us (our key=%1, keys present=%2)")
                         .arg(m_identityKey).arg(QStringList(ciphertextMap.keys()).join(",")));
        return;
    }

    QVariantMap myCipher = ciphertextMap.value(m_identityKey).toMap();
    int msgType = myCipher.value("type").toInt();
    QString body = myCipher.value("body").toString();
    if (body.isEmpty()) { appendOlmLog("  empty ciphertext body"); return; }

    QString plaintext;
    if (!olmDecryptFrom(senderKey, msgType, body, &plaintext)) {
        appendOlmLog(QString("  olmDecryptFrom failed, senderKey=%1 msgType=%2").arg(senderKey).arg(msgType));
        return;
    }

    JsonDataAccess jda;
    QVariant parsed = jda.loadFromBuffer(plaintext.toUtf8());
    if (jda.hasError()) { appendOlmLog(QString("  inner JSON parse failed: %1").arg(plaintext.left(300))); return; }

    QVariantMap inner = parsed.toMap();
    appendOlmLog(QString("  decrypted inner type=%1").arg(inner.value("type").toString()));
    if (inner.value("type").toString() != "m.room_key") return;

    QVariantMap roomKeyContent = inner.value("content").toMap();
    if (roomKeyContent.value("algorithm").toString() != "m.megolm.v1.aes-sha2") {
        appendOlmLog(QString("  room_key wrong algorithm: %1").arg(roomKeyContent.value("algorithm").toString()));
        return;
    }

    QString roomId = roomKeyContent.value("room_id").toString();
    QString sessionId = roomKeyContent.value("session_id").toString();
    QString sessionKey = roomKeyContent.value("session_key").toString();
    if (roomId.isEmpty() || sessionId.isEmpty() || sessionKey.isEmpty()) {
        appendOlmLog("  missing roomId/sessionId/sessionKey in room_key content");
        return;
    }

    appendOlmLog(QString("  importLiveSession room=%1 session=%2").arg(roomId).arg(sessionId));
    m_keyBackup->importLiveSession(roomId, sessionId, sessionKey);
}

void *OlmCryptoManager::olmSessionFor(const QString &theirIdentityKey) const
{
    return m_olmSessions.value(theirIdentityKey, 0);
}

void OlmCryptoManager::storeOlmSession(const QString &theirIdentityKey, void *session)
{
    if (void *old = m_olmSessions.value(theirIdentityKey)) {
        olm_clear_session(static_cast<OlmSession*>(old));
        std::free(old);
    }
    m_olmSessions[theirIdentityKey] = session;
}

bool OlmCryptoManager::olmDecryptFrom(const QString &senderIdentityKey, int messageType, const QString &ciphertextB64, QString *outPlaintext)
{
    OlmAccount *account = static_cast<OlmAccount*>(m_account);
    void *sessionMem = olmSessionFor(senderIdentityKey);
    OlmSession *session = 0;

    if (messageType == 0) {
        if (sessionMem) {
            QByteArray probe = ciphertextB64.toUtf8();
            size_t matches = olm_matches_inbound_session(static_cast<OlmSession*>(sessionMem), probe.data(), probe.size());
            if (matches == 1) session = static_cast<OlmSession*>(sessionMem);
        }
        if (!session) {
            void *mem = std::malloc(olm_session_size());
            OlmSession *newSession = olm_session(mem);
            QByteArray createBuf = ciphertextB64.toUtf8();
            size_t res = olm_create_inbound_session(newSession, account, createBuf.data(), createBuf.size());
            if (res == olm_error()) {
                olm_clear_session(newSession);
                std::free(mem);
                return false;
            }
            olm_remove_one_time_keys(account, newSession);
            storeOlmSession(senderIdentityKey, mem);
            persistAccount();
            session = newSession;
        }
    } else {
        if (!sessionMem) return false;
        session = static_cast<OlmSession*>(sessionMem);
    }

    QByteArray probeLen = ciphertextB64.toUtf8();
    size_t maxLen = olm_decrypt_max_plaintext_length(session, (size_t)messageType, probeLen.data(), probeLen.size());
    if (maxLen == olm_error()) return false;

    QByteArray decryptBuf = ciphertextB64.toUtf8();
    QByteArray plaintext(int(maxLen), '\0');
    size_t plainLen = olm_decrypt(session, (size_t)messageType, decryptBuf.data(), decryptBuf.size(), plaintext.data(), plaintext.size());
    if (plainLen == olm_error()) return false;

    plaintext.truncate(int(plainLen));
    *outPlaintext = QString::fromUtf8(plaintext);
    return true;
}

QVariantMap OlmCryptoManager::buildOlmPlaintext(const QString &type, const QVariantMap &content, const QString &recipientUserId, const QString &recipientEd25519Key) const
{
    QVariantMap obj;
    obj["type"] = type;
    obj["content"] = content;
    obj["sender"] = m_api->userId();
    obj["sender_device"] = m_deviceId;
    QVariantMap keys;
    keys["ed25519"] = m_fingerprintKey;
    obj["keys"] = keys;
    obj["recipient"] = recipientUserId;
    QVariantMap recipientKeys;
    recipientKeys["ed25519"] = recipientEd25519Key;
    obj["recipient_keys"] = recipientKeys;
    return obj;
}

bool OlmCryptoManager::olmEncryptFor(const QString &theirIdentityKey, const QVariantMap &plaintextObj, int *outType, QString *outCiphertext)
{
    void *sessionMem = olmSessionFor(theirIdentityKey);
    if (!sessionMem) return false;
    OlmSession *session = static_cast<OlmSession*>(sessionMem);

    JsonDataAccess jda;
    QByteArray plaintext;
    jda.saveToBuffer(QVariant(plaintextObj), &plaintext);

    size_t msgType = olm_encrypt_message_type(session);
    if (msgType == olm_error()) return false;
    *outType = int(msgType);

    size_t randLen = olm_encrypt_random_length(session);
    QByteArray random = randomBytes(int(randLen));
    size_t msgLen = olm_encrypt_message_length(session, plaintext.size());
    QByteArray message(int(msgLen), '\0');

    size_t res = olm_encrypt(session, plaintext.constData(), plaintext.size(),
            random.data(), random.size(), message.data(), message.size());
    if (res == olm_error()) return false;
    message.truncate(int(res));
    *outCiphertext = QString::fromUtf8(message);
    return true;
}

OlmCryptoManager::RoomCrypto &OlmCryptoManager::roomState(const QString &roomId)
{
    return m_rooms[roomId];
}

void *OlmCryptoManager::ensureOutboundSession(const QString &roomId, RoomCrypto &state)
{
    if (state.outboundSession) return state.outboundSession;

    void *mem = std::malloc(olm_outbound_group_session_size());
    OlmOutboundGroupSession *session = olm_outbound_group_session(mem);
    size_t randLen = olm_init_outbound_group_session_random_length(session);
    QByteArray random = randomBytes(int(randLen));
    size_t res = olm_init_outbound_group_session(session, (uint8_t*)random.data(), random.size());
    if (res == olm_error()) {
        olm_clear_outbound_group_session(session);
        std::free(mem);
        return 0;
    }

    QByteArray idBuf(int(olm_outbound_group_session_id_length(session)), '\0');
    olm_outbound_group_session_id(session, (uint8_t*)idBuf.data(), idBuf.size());

    state.outboundSession = mem;
    state.sessionId = QString::fromUtf8(idBuf);
    state.sharedTo.clear();

    // Register this same session as an inbound session too (at message
    // index 0, before anything has been encrypted with it), so our own
    // messages come back decryptable via the normal /sync path when the
    // server echoes them back to us -- otherwise Beport would show its own
    // sent messages as an undecryptable placeholder.
    QByteArray keyBuf(int(olm_outbound_group_session_key_length(session)), '\0');
    olm_outbound_group_session_key(session, (uint8_t*)keyBuf.data(), keyBuf.size());
    m_keyBackup->importLiveSession(roomId, state.sessionId, QString::fromUtf8(keyBuf));

    return mem;
}

void OlmCryptoManager::failPending(const QString &roomId, const QString &error)
{
    RoomCrypto &state = roomState(roomId);
    QList<PendingSend> toFail = state.pending;
    state.pending.clear();
    for (int i = 0; i < toFail.size(); ++i) emit sendFailed(toFail.at(i).txnId, error);
}

void OlmCryptoManager::encryptAndSend(const QString &roomId, const QVariantMap &content, const QString &txnId, const QString &eventType)
{
    RoomCrypto &state = roomState(roomId);
    PendingSend ps;
    ps.content = content;
    ps.txnId = txnId;
    ps.eventType = eventType;
    state.pending.append(ps);
    ensureRoomReady(roomId);
}

void OlmCryptoManager::ensureRoomReady(const QString &roomId)
{
    RoomCrypto &state = roomState(roomId);
    if (state.queryInFlight) return;

    if (!state.membersKnown) {
        state.queryInFlight = true;
        QString path = QString("/rooms/%1/joined_members").arg(QString(QUrl::toPercentEncoding(roomId)));
        QNetworkReply *reply = m_api->apiGet(path);
        m_joinedMembersRoom[reply] = roomId;
        connect(reply, SIGNAL(finished()), this, SLOT(onJoinedMembersReplyFinished()));
        return;
    }

    if (!state.devicesKnown) {
        state.queryInFlight = true;
        QVariantMap deviceKeysReq;
        for (int i = 0; i < state.memberIds.size(); ++i) deviceKeysReq[state.memberIds.at(i)] = QVariantList();
        QVariantMap body;
        body["device_keys"] = deviceKeysReq;
        QNetworkReply *reply = m_api->apiPost("/keys/query", body);
        m_keysQueryRoom[reply] = roomId;
        connect(reply, SIGNAL(finished()), this, SLOT(onKeysQueryReplyFinished()));
        return;
    }

    shareKeyAndFlush(roomId);
}

void OlmCryptoManager::onJoinedMembersReplyFinished()
{
    QNetworkReply *reply = qobject_cast<QNetworkReply*>(sender());
    if (!reply) return;
    QString roomId = m_joinedMembersRoom.take(reply);
    bool ok = false;
    QVariant parsed = MatrixApi::parseJson(reply, &ok);
    reply->deleteLater();

    RoomCrypto &state = roomState(roomId);
    state.queryInFlight = false;

    if (!ok) {
        failPending(roomId, "Couldn't read room members.");
        return;
    }

    QVariantMap joined = parsed.toMap().value("joined").toMap();
    state.memberIds = joined.keys();
    state.membersKnown = true;
    ensureRoomReady(roomId);
}

void OlmCryptoManager::onKeysQueryReplyFinished()
{
    QNetworkReply *reply = qobject_cast<QNetworkReply*>(sender());
    if (!reply) return;
    QString roomId = m_keysQueryRoom.take(reply);
    bool ok = false;
    QVariant parsed = MatrixApi::parseJson(reply, &ok);
    reply->deleteLater();

    RoomCrypto &state = roomState(roomId);
    state.queryInFlight = false;

    if (!ok) {
        failPending(roomId, "Couldn't read device keys.");
        return;
    }

    mergeDeviceKeysResponse(parsed.toMap(), state.devices);
    mergeDeviceKeysResponse(parsed.toMap(), m_deviceKeys);
    state.devicesKnown = true;
    ensureRoomReady(roomId);
}

void OlmCryptoManager::mergeDeviceKeysResponse(const QVariantMap &parsed, QHash<QString, QHash<QString, QVariantMap> > &target)
{
    QVariantMap deviceKeysByUser = parsed.value("device_keys").toMap();
    QMapIterator<QString, QVariant> userIt(deviceKeysByUser);
    while (userIt.hasNext()) {
        userIt.next();
        QString userId = userIt.key();
        QVariantMap devicesForUser = userIt.value().toMap();
        QMapIterator<QString, QVariant> devIt(devicesForUser);
        while (devIt.hasNext()) {
            devIt.next();
            QString deviceId = devIt.key();
            QVariantMap deviceInfo = devIt.value().toMap();
            QVariantMap keys = deviceInfo.value("keys").toMap();
            QVariantMap entry;
            entry["curve25519"] = keys.value(QString("curve25519:%1").arg(deviceId)).toString();
            entry["ed25519"] = keys.value(QString("ed25519:%1").arg(deviceId)).toString();
            if (entry.value("curve25519").toString().isEmpty() || entry.value("ed25519").toString().isEmpty()) continue;
            target[userId][deviceId] = entry;
        }
    }
}

void OlmCryptoManager::shareKeyAndFlush(const QString &roomId)
{
    RoomCrypto &state = roomState(roomId);

    if (!ensureOutboundSession(roomId, state)) {
        failPending(roomId, "Couldn't create the Megolm session.");
        return;
    }

    QVariantMap needClaim; // userId -> {deviceId: "signed_curve25519"}
    QStringList needShare; // "userId|deviceId" needing the room key

    QHashIterator<QString, QHash<QString, QVariantMap> > userIt(state.devices);
    while (userIt.hasNext()) {
        userIt.next();
        QString userId = userIt.key();
        QHashIterator<QString, QVariantMap> devIt(userIt.value());
        while (devIt.hasNext()) {
            devIt.next();
            QString deviceId = devIt.key();
            if (userId == m_api->userId() && deviceId == m_deviceId) continue;
            QString tag = userId + "|" + deviceId;
            if (state.sharedTo.contains(tag)) continue;
            needShare << tag;

            QString curveKey = devIt.value().value("curve25519").toString();
            if (!olmSessionFor(curveKey)) {
                QVariantMap perUser = needClaim.value(userId).toMap();
                perUser[deviceId] = "signed_curve25519";
                needClaim[userId] = perUser;
            }
        }
    }

    if (needShare.isEmpty()) {
        flushPendingSends(roomId);
        return;
    }

    if (!needClaim.isEmpty()) {
        QVariantMap body;
        body["one_time_keys"] = needClaim;
        QNetworkReply *reply = m_api->apiPost("/keys/claim", body);
        m_keysClaimRoom[reply] = roomId;
        connect(reply, SIGNAL(finished()), this, SLOT(onKeysClaimReplyFinished()));
        return;
    }

    sendRoomKeyToDevices(roomId, needShare);
}

void OlmCryptoManager::onKeysClaimReplyFinished()
{
    QNetworkReply *reply = qobject_cast<QNetworkReply*>(sender());
    if (!reply) return;
    QString roomId = m_keysClaimRoom.take(reply);
    bool ok = false;
    QVariant parsed = MatrixApi::parseJson(reply, &ok);
    reply->deleteLater();

    if (!ok) {
        failPending(roomId, "Couldn't request one-time keys.");
        return;
    }

    RoomCrypto &state = roomState(roomId);
    QVariantMap oneTimeKeys = parsed.toMap().value("one_time_keys").toMap();
    OlmAccount *account = static_cast<OlmAccount*>(m_account);

    QStringList readyToShare;
    QHashIterator<QString, QHash<QString, QVariantMap> > userIt(state.devices);
    while (userIt.hasNext()) {
        userIt.next();
        QString userId = userIt.key();
        QHashIterator<QString, QVariantMap> devIt(userIt.value());
        while (devIt.hasNext()) {
            devIt.next();
            QString deviceId = devIt.key();
            if (userId == m_api->userId() && deviceId == m_deviceId) continue;
            QString tag = userId + "|" + deviceId;
            if (state.sharedTo.contains(tag)) continue;

            QString curveKey = devIt.value().value("curve25519").toString();
            if (olmSessionFor(curveKey)) {
                readyToShare << tag;
                continue;
            }

            QVariantMap deviceClaimed = oneTimeKeys.value(userId).toMap().value(deviceId).toMap();
            if (deviceClaimed.isEmpty()) continue; // no one-time key available for this device

            QMapIterator<QString, QVariant> keyIt(deviceClaimed);
            if (!keyIt.hasNext()) continue;
            keyIt.next();
            QString otk = keyIt.value().toMap().value("key").toString();
            if (otk.isEmpty()) continue;

            void *mem = std::malloc(olm_session_size());
            OlmSession *session = olm_session(mem);
            QByteArray theirId = curveKey.toUtf8();
            QByteArray theirOtk = otk.toUtf8();
            size_t randLen = olm_create_outbound_session_random_length(session);
            QByteArray random = randomBytes(int(randLen));
            size_t res = olm_create_outbound_session(session, account,
                    theirId.constData(), theirId.size(),
                    theirOtk.constData(), theirOtk.size(),
                    random.data(), random.size());
            if (res == olm_error()) {
                olm_clear_session(session);
                std::free(mem);
                continue;
            }
            storeOlmSession(curveKey, mem);
            readyToShare << tag;
        }
    }

    if (readyToShare.isEmpty()) {
        flushPendingSends(roomId);
        return;
    }
    sendRoomKeyToDevices(roomId, readyToShare);
}

void OlmCryptoManager::sendRoomKeyToDevices(const QString &roomId, const QStringList &deviceTags)
{
    RoomCrypto &state = roomState(roomId);
    if (!state.outboundSession) {
        failPending(roomId, "Sessione di stanza mancante.");
        return;
    }
    OlmOutboundGroupSession *session = static_cast<OlmOutboundGroupSession*>(state.outboundSession);

    QByteArray keyBuf(int(olm_outbound_group_session_key_length(session)), '\0');
    olm_outbound_group_session_key(session, (uint8_t*)keyBuf.data(), keyBuf.size());

    QVariantMap roomKeyContent;
    roomKeyContent["algorithm"] = "m.megolm.v1.aes-sha2";
    roomKeyContent["room_id"] = roomId;
    roomKeyContent["session_id"] = state.sessionId;
    roomKeyContent["session_key"] = QString::fromUtf8(keyBuf);

    QVariantMap messages; // userId -> deviceId -> encrypted content
    QStringList actuallyShared;

    for (int i = 0; i < deviceTags.size(); ++i) {
        QStringList parts = deviceTags.at(i).split("|");
        if (parts.size() != 2) continue;
        QString userId = parts.at(0);
        QString deviceId = parts.at(1);
        QVariantMap devInfo = state.devices.value(userId).value(deviceId);
        QString curveKey = devInfo.value("curve25519").toString();
        QString ed25519Key = devInfo.value("ed25519").toString();
        if (curveKey.isEmpty() || !olmSessionFor(curveKey)) continue;

        QVariantMap plaintextObj = buildOlmPlaintext("m.room_key", roomKeyContent, userId, ed25519Key);
        int msgType = 0;
        QString ciphertext;
        if (!olmEncryptFor(curveKey, plaintextObj, &msgType, &ciphertext)) continue;

        QVariantMap cipherEntry;
        cipherEntry["type"] = msgType;
        cipherEntry["body"] = ciphertext;
        QVariantMap cipherMap;
        cipherMap[curveKey] = cipherEntry;

        QVariantMap encryptedContent;
        encryptedContent["algorithm"] = "m.olm.v1.curve25519-aes-sha2";
        encryptedContent["sender_key"] = m_identityKey;
        encryptedContent["ciphertext"] = cipherMap;

        QVariantMap perUser = messages.value(userId).toMap();
        perUser[deviceId] = encryptedContent;
        messages[userId] = perUser;
        actuallyShared << deviceTags.at(i);
    }

    if (messages.isEmpty()) {
        flushPendingSends(roomId);
        return;
    }

    QVariantMap body;
    body["messages"] = messages;
    QString txnId = m_api->nextTxnId();
    QString path = QString("/sendToDevice/m.room.encrypted/%1").arg(QString(QUrl::toPercentEncoding(txnId)));
    QNetworkReply *reply = m_api->apiPut(path, body);
    m_sendToDeviceRoom[reply] = roomId;
    m_sendToDeviceShared[reply] = actuallyShared;
    connect(reply, SIGNAL(finished()), this, SLOT(onSendToDeviceReplyFinished()));
}

void OlmCryptoManager::onSendToDeviceReplyFinished()
{
    QNetworkReply *reply = qobject_cast<QNetworkReply*>(sender());
    if (!reply) return;
    QString roomId = m_sendToDeviceRoom.take(reply);
    QStringList shared = m_sendToDeviceShared.take(reply);
    bool ok = false;
    MatrixApi::parseJson(reply, &ok);
    reply->deleteLater();

    RoomCrypto &state = roomState(roomId);
    if (ok) {
        for (int i = 0; i < shared.size(); ++i) state.sharedTo.insert(shared.at(i));
    }
    flushPendingSends(roomId);
}

void OlmCryptoManager::flushPendingSends(const QString &roomId)
{
    RoomCrypto &state = roomState(roomId);
    if (!state.outboundSession) {
        failPending(roomId, "Encryption session not available.");
        return;
    }
    OlmOutboundGroupSession *session = static_cast<OlmOutboundGroupSession*>(state.outboundSession);

    QList<PendingSend> toSend = state.pending;
    state.pending.clear();

    for (int i = 0; i < toSend.size(); ++i) {
        QVariantMap plaintextEvent;
        plaintextEvent["type"] = toSend.at(i).eventType.isEmpty() ? QString("m.room.message") : toSend.at(i).eventType;
        plaintextEvent["content"] = toSend.at(i).content;
        plaintextEvent["room_id"] = roomId;

        JsonDataAccess jda;
        QByteArray plaintext;
        jda.saveToBuffer(QVariant(plaintextEvent), &plaintext);

        size_t msgLen = olm_group_encrypt_message_length(session, plaintext.size());
        QByteArray message(int(msgLen), '\0');
        size_t res = olm_group_encrypt(session, (const uint8_t*)plaintext.constData(), plaintext.size(),
                (uint8_t*)message.data(), message.size());
        if (res == olm_error()) {
            emit sendFailed(toSend.at(i).txnId, "Message encryption failed.");
            continue;
        }
        message.truncate(int(res));

        QVariantMap encryptedContent;
        encryptedContent["algorithm"] = "m.megolm.v1.aes-sha2";
        encryptedContent["sender_key"] = m_identityKey;
        encryptedContent["ciphertext"] = QString::fromUtf8(message);
        encryptedContent["session_id"] = state.sessionId;
        encryptedContent["device_id"] = m_deviceId;

        QString path = QString("/rooms/%1/send/m.room.encrypted/%2")
                .arg(QString(QUrl::toPercentEncoding(roomId)))
                .arg(QString(QUrl::toPercentEncoding(toSend.at(i).txnId)));
        QNetworkReply *reply = m_api->apiPut(path, encryptedContent);
        m_encryptedSendTxn[reply] = toSend.at(i).txnId;
        m_encryptedSendRoom[reply] = roomId;
        connect(reply, SIGNAL(finished()), this, SLOT(onEncryptedSendReplyFinished()));
    }
}

void OlmCryptoManager::onEncryptedSendReplyFinished()
{
    QNetworkReply *reply = qobject_cast<QNetworkReply*>(sender());
    if (!reply) return;
    QString txnId = m_encryptedSendTxn.take(reply);
    m_encryptedSendRoom.take(reply);
    bool ok = false;
    QVariant parsed = MatrixApi::parseJson(reply, &ok);
    reply->deleteLater();

    if (!ok || !parsed.toMap().contains("event_id")) {
        emit sendFailed(txnId, "Failed to send encrypted message.");
        return;
    }
    emit sendSucceeded(txnId);
}

// --- Interactive SAS device verification -----------------------------
//
// Beport always plays the requester/starter role: it sends .request then
// .start, and only ever proceeds past .ready if IT sent the original
// .request (checked via m_verification.active + matching transaction_id).
// It never responds to a verification someone else initiated.
//
// Info-string formats and the commitment formula below were cross-checked
// against matrix-nio's real, interoperable implementation (nio/crypto/sas.py)
// rather than reconstructed from memory, since a single wrong character here
// makes both sides silently derive different SAS/MAC values with no clear
// error -- exactly the "Mismatched commitment" class of bug seen in other
// third-party Matrix SAS implementations.

void OlmCryptoManager::sendToOneDevice(const QString &eventType, const QString &userId, const QString &deviceId, const QVariantMap &content)
{
    QVariantMap perDevice;
    perDevice[deviceId] = content;
    QVariantMap messages;
    messages[userId] = perDevice;
    QVariantMap body;
    body["messages"] = messages;

    QString path = QString("/sendToDevice/%1/%2")
            .arg(QString(QUrl::toPercentEncoding(eventType)))
            .arg(QString(QUrl::toPercentEncoding(m_api->nextTxnId())));
    QNetworkReply *reply = m_api->apiPut(path, body);
    connect(reply, SIGNAL(finished()), reply, SLOT(deleteLater()));
}

QString OlmCryptoManager::sasInfo() const
{
    // The SAS bytes must come out identical on both sides, so the ordering
    // is pinned to who objectively sent .start (matches nio: "if
    // we_started_it: our_info first"), not to whichever side is computing
    // it right now.
    QString ourInfo = QString("%1|%2|%3").arg(m_api->userId(), m_deviceId, m_verification.ourPubkey);
    QString theirInfo = QString("%1|%2|%3").arg(m_verification.theirUserId, m_verification.theirDeviceId, m_verification.theirPubkey);
    if (m_verification.weStarted) {
        return QString("MATRIX_KEY_VERIFICATION_SAS|%1|%2|%3").arg(ourInfo, theirInfo, m_verification.transactionId);
    }
    return QString("MATRIX_KEY_VERIFICATION_SAS|%1|%2|%3").arg(theirInfo, ourInfo, m_verification.transactionId);
}

// Two distinct info strings are needed, not one: a MAC authenticates a
// SPECIFIC device's key, and its info string is pinned to that device
// (MAC-owner first, then the other party), regardless of which side is
// generating it (to send) vs recomputing it (to verify what it received).
QString OlmCryptoManager::macInfoForOurKey() const
{
    return QString("MATRIX_KEY_VERIFICATION_MAC%1%2%3%4%5")
            .arg(m_api->userId(), m_deviceId, m_verification.theirUserId, m_verification.theirDeviceId, m_verification.transactionId);
}

QString OlmCryptoManager::macInfoForTheirKey() const
{
    return QString("MATRIX_KEY_VERIFICATION_MAC%1%2%3%4%5")
            .arg(m_verification.theirUserId, m_verification.theirDeviceId, m_api->userId(), m_deviceId, m_verification.transactionId);
}

void OlmCryptoManager::startVerification()
{
    if (m_verification.active) return;
    m_verification = VerificationState();

    if (m_deviceKeys.contains(m_api->userId())) {
        sendVerificationRequest();
        return;
    }

    QVariantMap deviceKeysReq;
    deviceKeysReq[m_api->userId()] = QVariantList();
    QVariantMap body;
    body["device_keys"] = deviceKeysReq;
    QNetworkReply *reply = m_api->apiPost("/keys/query", body);
    connect(reply, SIGNAL(finished()), this, SLOT(onVerificationKeysQueryReplyFinished()));
    setVerificationStatus("Ricerca dei tuoi altri dispositivi...");
}

void OlmCryptoManager::onVerificationKeysQueryReplyFinished()
{
    QNetworkReply *reply = qobject_cast<QNetworkReply*>(sender());
    if (!reply) return;
    bool ok = false;
    QVariant parsed = MatrixApi::parseJson(reply, &ok);
    reply->deleteLater();
    if (!ok) {
        setVerificationStatus("Couldn't read the account's devices.");
        return;
    }
    mergeDeviceKeysResponse(parsed.toMap(), m_deviceKeys);

    // Distinguish which flow triggered this query: accepting an incoming
    // verification already has theirUserId/theirDeviceId populated (set
    // when the .start event arrived); starting one fresh does not yet.
    if (m_verification.active && !m_verification.theirDeviceId.isEmpty()) {
        sendVerificationAccept();
    } else {
        sendVerificationRequest();
    }
}

void OlmCryptoManager::sendVerificationRequest()
{
    // Skip the newer .request -> .ready handshake (whose sole purpose is
    // letting the initiator discover a target device it doesn't already
    // know) and go straight to .start against a device we already know
    // about from /keys/query. This is the original, universally-supported
    // SAS entry point; the .request/.ready dance is a newer addition (for
    // "verify with any of my devices, whichever answers") that several
    // real-world clients -- observed here with Beeper Desktop, confirmed
    // via matrix-rust-sdk's own historical "receiving a verification
    // request does not work" bug -- don't reliably surface to the user.
    QHash<QString, QVariantMap> ourDevices = m_deviceKeys.value(m_api->userId());
    QString targetDeviceId;
    QHashIterator<QString, QVariantMap> it(ourDevices);
    while (it.hasNext()) {
        it.next();
        if (it.key() == m_deviceId) continue;
        targetDeviceId = it.key();
        break;
    }

    if (targetDeviceId.isEmpty()) {
        setVerificationStatus("No other device found on this account.");
        return;
    }

    m_verification.transactionId = m_api->nextTxnId();
    m_verification.active = true;
    m_verification.theirUserId = m_api->userId();
    m_verification.theirDeviceId = targetDeviceId;

    sendVerificationStart();
}

void OlmCryptoManager::sendVerificationStart()
{
    void *mem = std::malloc(olm_sas_size());
    OlmSAS *sas = olm_sas(mem);
    size_t randLen = olm_create_sas_random_length(sas);
    QByteArray random = randomBytes(int(randLen));
    olm_create_sas(sas, random.data(), random.size());
    m_verification.sas = mem;

    QByteArray pubkeyBuf(int(olm_sas_pubkey_length(sas)), '\0');
    olm_sas_get_pubkey(sas, pubkeyBuf.data(), pubkeyBuf.size());
    m_verification.ourPubkey = QString::fromUtf8(pubkeyBuf);

    QVariantMap content;
    content["from_device"] = m_deviceId;
    content["method"] = "m.sas.v1";
    QVariantList kaProtocols;
    kaProtocols << "curve25519-hkdf-sha256";
    content["key_agreement_protocols"] = kaProtocols;
    QVariantList hashes;
    hashes << "sha256";
    content["hashes"] = hashes;
    QVariantList macs;
    macs << "hkdf-hmac-sha256.v2";
    content["message_authentication_codes"] = macs;
    QVariantList sasMethods;
    sasMethods << "decimal" << "emoji";
    content["short_authentication_string"] = sasMethods;
    content["transaction_id"] = m_verification.transactionId;

    m_verification.weStarted = true;
    m_verification.startContent = content;
    sendToOneDevice("m.key.verification.start", m_verification.theirUserId, m_verification.theirDeviceId, content);
    setVerificationStatus("Verification started, waiting for a reply...");
}

void OlmCryptoManager::sendVerificationAccept()
{
    void *mem = std::malloc(olm_sas_size());
    OlmSAS *sas = olm_sas(mem);
    size_t randLen = olm_create_sas_random_length(sas);
    QByteArray random = randomBytes(int(randLen));
    olm_create_sas(sas, random.data(), random.size());
    m_verification.sas = mem;

    QByteArray pubkeyBuf(int(olm_sas_pubkey_length(sas)), '\0');
    olm_sas_get_pubkey(sas, pubkeyBuf.data(), pubkeyBuf.size());
    m_verification.ourPubkey = QString::fromUtf8(pubkeyBuf);

    // Commit to our (not-yet-revealed) pubkey + the .start content we
    // received, so the starter can catch us out later if we try to swap
    // keys after seeing theirs.
    QByteArray toHash = m_verification.ourPubkey.toUtf8() + canonicalJson(m_verification.startContent);
    void *utilMem = std::malloc(olm_utility_size());
    OlmUtility *util = olm_utility(utilMem);
    QByteArray commitment(int(olm_sha256_length(util)), '\0');
    olm_sha256(util, toHash.constData(), toHash.size(), commitment.data(), commitment.size());
    olm_clear_utility(util);
    std::free(utilMem);

    QVariantMap content;
    content["transaction_id"] = m_verification.transactionId;
    content["method"] = "m.sas.v1";
    content["key_agreement_protocol"] = "curve25519-hkdf-sha256";
    content["hash"] = "sha256";
    content["message_authentication_code"] = "hkdf-hmac-sha256.v2";
    QVariantList sasMethods;
    sasMethods << "decimal" << "emoji";
    content["short_authentication_string"] = sasMethods;
    content["commitment"] = QString::fromUtf8(commitment);

    sendToOneDevice("m.key.verification.accept", m_verification.theirUserId, m_verification.theirDeviceId, content);
    setVerificationStatus("Verification accepted, waiting for the other device's key...");
}

void OlmCryptoManager::sendVerificationKey()
{
    QVariantMap content;
    content["transaction_id"] = m_verification.transactionId;
    content["key"] = m_verification.ourPubkey;
    sendToOneDevice("m.key.verification.key", m_verification.theirUserId, m_verification.theirDeviceId, content);
}

void OlmCryptoManager::sendVerificationMac()
{
    // olm_sas_calculate_mac_fixed_base64(), not plain olm_sas_calculate_mac():
    // libolm ships both, and they produce DIFFERENT MACs for the same input.
    // The plain one implements "hkdf-hmac-sha256" (v1); the "_fixed_base64"
    // one implements "hkdf-hmac-sha256.v2" (confirmed against matrix-js-sdk's
    // own macMethods table). We only ever negotiate ".v2", so using the
    // plain function here was a real interop bug -- Element rejected every
    // MAC with "keys MAC doesn't match" until this was caught.
    OlmSAS *sas = static_cast<OlmSAS*>(m_verification.sas);
    QString info = macInfoForOurKey();

    QString ourKeyId = QString("ed25519:%1").arg(m_deviceId);
    QByteArray fpInput = m_fingerprintKey.toUtf8();
    QByteArray fpInfoBytes = (info + ourKeyId).toUtf8();
    QByteArray fpMac(int(olm_sas_mac_length(sas)), '\0');
    olm_sas_calculate_mac_fixed_base64(sas, fpInput.constData(), fpInput.size(), fpInfoBytes.constData(), fpInfoBytes.size(), fpMac.data(), fpMac.size());

    QStringList keyIds;
    keyIds << ourKeyId;
    keyIds.sort();
    QByteArray keysMacInput = keyIds.join(",").toUtf8();
    QByteArray keysMacInfoBytes = (info + "KEY_IDS").toUtf8();
    QByteArray keysMac(int(olm_sas_mac_length(sas)), '\0');
    olm_sas_calculate_mac_fixed_base64(sas, keysMacInput.constData(), keysMacInput.size(), keysMacInfoBytes.constData(), keysMacInfoBytes.size(), keysMac.data(), keysMac.size());

    QVariantMap macMap;
    macMap[ourKeyId] = QString::fromUtf8(fpMac);

    QVariantMap content;
    content["transaction_id"] = m_verification.transactionId;
    content["mac"] = macMap;
    content["keys"] = QString::fromUtf8(keysMac);
    sendToOneDevice("m.key.verification.mac", m_verification.theirUserId, m_verification.theirDeviceId, content);
}

void OlmCryptoManager::sendVerificationDone()
{
    QVariantMap content;
    content["transaction_id"] = m_verification.transactionId;
    sendToOneDevice("m.key.verification.done", m_verification.theirUserId, m_verification.theirDeviceId, content);
}

void OlmCryptoManager::sendVerificationCancel(const QString &reason, const QString &code)
{
    if (m_verification.active && !m_verification.theirDeviceId.isEmpty()) {
        QVariantMap content;
        content["transaction_id"] = m_verification.transactionId;
        content["reason"] = reason;
        content["code"] = code;
        sendToOneDevice("m.key.verification.cancel", m_verification.theirUserId, m_verification.theirDeviceId, content);
    }
    setVerificationStatus("Verification cancelled: " + reason);
    setVerificationAwaitingConfirm(false);
    setVerificationSas(QString());
    setVerificationEmoji(QString());
    freeVerificationSas();
    m_verification = VerificationState();
    setVerificationIncoming(false, QString());
}

void OlmCryptoManager::confirmVerification()
{
    if (!m_verification.active || !m_verificationAwaitingConfirm) return;
    sendVerificationMac();
    setVerificationAwaitingConfirm(false);
    setVerificationStatus("Numbers confirmed, waiting for the other device...");
}

void OlmCryptoManager::cancelVerification()
{
    if (!m_verification.active) return;
    sendVerificationCancel("Cancelled by user.", "m.user");
}

void OlmCryptoManager::acceptIncomingVerification()
{
    if (!m_verification.active || !m_verification.incomingPending) return;
    setVerificationIncoming(false, QString());

    if (m_verification.startContent.isEmpty()) {
        // What's pending is an m.key.verification.request (the modern
        // "verify this new session?" flow other clients like Element use),
        // not a bare .start -- we don't have their SAS parameters yet.
        // Acknowledge with .ready and wait for them to follow up with the
        // actual .start, handled back in handleVerificationEvent().
        sendVerificationReady();
        return;
    }

    ensureTheirKeysThenAccept();
}

void OlmCryptoManager::ensureTheirKeysThenAccept()
{
    if (m_deviceKeys.value(m_verification.theirUserId).contains(m_verification.theirDeviceId)) {
        sendVerificationAccept();
        return;
    }

    QVariantMap deviceKeysReq;
    deviceKeysReq[m_verification.theirUserId] = QVariantList();
    QVariantMap body;
    body["device_keys"] = deviceKeysReq;
    QNetworkReply *reply = m_api->apiPost("/keys/query", body);
    connect(reply, SIGNAL(finished()), this, SLOT(onVerificationKeysQueryReplyFinished()));
    setVerificationStatus("Verifying device keys...");
}

void OlmCryptoManager::sendVerificationReady()
{
    QVariantMap content;
    content["transaction_id"] = m_verification.transactionId;
    content["from_device"] = m_deviceId;
    QVariantList methods;
    methods << "m.sas.v1";
    content["methods"] = methods;
    sendToOneDevice("m.key.verification.ready", m_verification.theirUserId, m_verification.theirDeviceId, content);
    setVerificationStatus("Request accepted, waiting for verification to start...");
}

void OlmCryptoManager::handleVerificationEvent(const QVariantMap &event)
{
    QString type = event.value("type").toString();
    QString sender = event.value("sender").toString();
    if (sender != m_api->userId()) return; // only self-verification (own other devices) supported

    QVariantMap content = event.value("content").toMap();
    QString txnId = content.value("transaction_id").toString();

    if (type == "m.key.verification.request") {
        // The modern self-verification entry point: an already-trusted
        // device (e.g. Element's "was this you?" toast on a new login) asks
        // whether we want to verify, rather than committing straight to
        // .start. We must reply .ready before it will send the real .start
        // -- without this branch, such requests were silently dropped and
        // the other side's flow just sat there waiting forever.
        if (m_verification.active) return; // already mid-flow elsewhere
        QString fromDevice = content.value("from_device").toString();
        QVariantList methods = content.value("methods").toList();
        if (fromDevice.isEmpty() || !methods.contains(QVariant("m.sas.v1"))) {
            return; // nothing we can do with this request; silently ignore
        }

        m_verification = VerificationState();
        m_verification.active = true;
        m_verification.weStarted = false;
        m_verification.transactionId = txnId;
        m_verification.theirUserId = sender;
        m_verification.theirDeviceId = fromDevice;
        // startContent stays empty: the actual .start (with SAS params)
        // only arrives after we accept and send .ready.

        setVerificationIncoming(true, fromDevice);
        setVerificationStatus(QString("Incoming verification request from %1.").arg(fromDevice));
        return;
    }

    if (type == "m.key.verification.start") {
        bool freshDirectStart = !m_verification.active;
        QString fromDevice = content.value("from_device").toString();
        bool followUpAfterReady = m_verification.active && !m_verification.weStarted
                && m_verification.startContent.isEmpty()
                && txnId == m_verification.transactionId
                && sender == m_verification.theirUserId
                && fromDevice == m_verification.theirDeviceId;
        if (!freshDirectStart && !followUpAfterReady) return;

        // Someone else (e.g. Element Web) started a verification against
        // us, either directly (legacy) or after we accepted their .request
        // via .ready. Validate we support what they're offering.
        QString method = content.value("method").toString();
        QVariantList kaProtocols = content.value("key_agreement_protocols").toList();
        QVariantList hashes = content.value("hashes").toList();
        QVariantList macs = content.value("message_authentication_codes").toList();
        QVariantList sasMethods = content.value("short_authentication_string").toList();
        if (method != "m.sas.v1" || fromDevice.isEmpty()
                || !kaProtocols.contains(QVariant("curve25519-hkdf-sha256"))
                || !hashes.contains(QVariant("sha256"))
                || !(macs.contains(QVariant("hkdf-hmac-sha256.v2")) || macs.contains(QVariant("hkdf-hmac-sha256")))
                || !(sasMethods.contains(QVariant("decimal")) || sasMethods.contains(QVariant("emoji")))) {
            if (followUpAfterReady) {
                // We already agreed to verify via .ready; a bad follow-up
                // .start deserves a cancel, not a silent, permanent stall.
                sendVerificationCancel("Verification method not supported by the other device.", "m.unknown_method");
            }
            return; // incompatible method offered
        }

        if (freshDirectStart) {
            m_verification = VerificationState();
            m_verification.active = true;
            m_verification.weStarted = false;
            m_verification.transactionId = txnId;
            m_verification.theirUserId = sender;
            m_verification.theirDeviceId = fromDevice;
        }
        m_verification.startContent = content;

        if (freshDirectStart) {
            setVerificationIncoming(true, fromDevice);
            setVerificationStatus(QString("Incoming verification request from %1.").arg(fromDevice));
        } else {
            // The user already accepted back when the .request came in;
            // proceed straight to .accept without asking again.
            setVerificationStatus("Verification started by the other device, checking keys...");
            ensureTheirKeysThenAccept();
        }
        return;
    }

    if (type == "m.key.verification.ready") {
        if (!m_verification.active || !m_verification.weStarted || !m_verification.theirDeviceId.isEmpty()) return;
        if (txnId != m_verification.transactionId) return;
        QVariantList methods = content.value("methods").toList();
        if (!methods.contains(QVariant("m.sas.v1"))) {
            sendVerificationCancel("No common verification method.", "m.unknown_method");
            return;
        }
        m_verification.theirDeviceId = content.value("from_device").toString();
        m_verification.theirUserId = sender;
        sendVerificationStart();
        return;
    }

    if (!m_verification.active || txnId != m_verification.transactionId) return;

    if (type == "m.key.verification.accept") {
        if (!m_verification.weStarted) return; // we don't send .accept-then-receive-another
        QString method = content.value("method").toString();
        QString kaProtocol = content.value("key_agreement_protocol").toString();
        QString hash = content.value("hash").toString();
        QVariantList sasMethods = content.value("short_authentication_string").toList();
        if (method != "m.sas.v1" || kaProtocol != "curve25519-hkdf-sha256" || hash != "sha256"
                || !(sasMethods.contains(QVariant("decimal")) || sasMethods.contains(QVariant("emoji")))) {
            sendVerificationCancel("Verification method not supported by the other device.", "m.unknown_method");
            return;
        }
        m_verification.theirCommitment = content.value("commitment").toString();
        sendVerificationKey();
        return;
    }

    if (type == "m.key.verification.key") {
        QString theirKey = content.value("key").toString();
        if (theirKey.isEmpty()) return;

        if (m_verification.weStarted) {
            // We sent .start; this is the accepter finally revealing the
            // key it committed to earlier. Check that commitment now.
            QByteArray toHash = theirKey.toUtf8() + canonicalJson(m_verification.startContent);
            void *utilMem = std::malloc(olm_utility_size());
            OlmUtility *util = olm_utility(utilMem);
            QByteArray hashOut(int(olm_sha256_length(util)), '\0');
            olm_sha256(util, toHash.constData(), toHash.size(), hashOut.data(), hashOut.size());
            olm_clear_utility(util);
            std::free(utilMem);

            if (QString::fromUtf8(hashOut) != m_verification.theirCommitment) {
                sendVerificationCancel("Commitment doesn't match: possible tampering.", "m.mismatched_commitment");
                return;
            }
        }

        m_verification.theirPubkey = theirKey;

        OlmSAS *sas = static_cast<OlmSAS*>(m_verification.sas);
        QByteArray theirKeyBuf = theirKey.toUtf8();
        size_t res = olm_sas_set_their_key(sas, theirKeyBuf.data(), theirKeyBuf.size());
        if (res == olm_error()) {
            sendVerificationCancel("Cryptographic error during key exchange.", "m.invalid_message");
            return;
        }

        if (!m_verification.weStarted) {
            // We're the accepter: our own key was only committed-to before,
            // safe to reveal now that we have theirs.
            sendVerificationKey();
        }

        // 6 bytes generated (not just the 5 "decimal" needs): olm's SAS byte
        // generation is a streaming HKDF-expand, so the first 5 bytes are
        // identical whether 5 or 6 are requested -- this just also gives us
        // the 6th byte "emoji" needs. Both are shown because peers differ on
        // which they display by default (Element defaults to emoji, shown
        // alongside "waiting for the other device" until the OTHER side
        // also confirms -- this isn't just cosmetic).
        QByteArray infoBytes = sasInfo().toUtf8();
        QByteArray sasBytes(6, '\0');
        olm_sas_generate_bytes(sas, infoBytes.constData(), infoBytes.size(), (uint8_t*)sasBytes.data(), sasBytes.size());

        const unsigned char *b = (const unsigned char*)sasBytes.constData();
        int n1 = (int(b[0]) << 5) | (int(b[1]) >> 3);
        int n2 = ((int(b[1]) & 0x7) << 10) | (int(b[2]) << 2) | (int(b[3]) >> 6);
        int n3 = ((int(b[3]) & 0x3F) << 7) | int(b[4]);
        n1 += 1000;
        n2 += 1000;
        n3 += 1000;

        setVerificationSas(QString("%1 - %2 - %3").arg(n1).arg(n2).arg(n3));
        setVerificationEmoji(emojiStringForSasBytes(b));
        setVerificationAwaitingConfirm(true);
        setVerificationStatus("Compare these numbers/emoji with what's shown on the other device.");
        return;
    }

    if (type == "m.key.verification.mac") {
        OlmSAS *sas = static_cast<OlmSAS*>(m_verification.sas);
        if (!sas) {
            sendVerificationCancel("Unexpected verification state.", "m.unexpected_message");
            return;
        }

        QVariantMap macMap = content.value("mac").toMap();
        QString keysMac = content.value("keys").toString();
        QString info = macInfoForTheirKey();

        QStringList keyIds = macMap.keys();
        keyIds.sort();
        QByteArray keysMacInput = keyIds.join(",").toUtf8();
        QByteArray keysMacInfoBytes = (info + "KEY_IDS").toUtf8();
        QByteArray computedKeysMac(int(olm_sas_mac_length(sas)), '\0');
        olm_sas_calculate_mac_fixed_base64(sas, keysMacInput.constData(), keysMacInput.size(), keysMacInfoBytes.constData(), keysMacInfoBytes.size(), computedKeysMac.data(), computedKeysMac.size());

        if (QString::fromUtf8(computedKeysMac) != keysMac) {
            sendVerificationCancel("Keys MAC doesn't match.", "m.key_mismatch");
            return;
        }

        QString expectedKeyId = QString("ed25519:%1").arg(m_verification.theirDeviceId);
        if (!macMap.contains(expectedKeyId)) {
            sendVerificationCancel("Expected key missing from MAC.", "m.key_mismatch");
            return;
        }

        QString theirFingerprint = m_deviceKeys.value(m_verification.theirUserId).value(m_verification.theirDeviceId).value("ed25519").toString();
        if (theirFingerprint.isEmpty()) {
            sendVerificationCancel("Unknown device fingerprint.", "m.key_mismatch");
            return;
        }

        QByteArray fpInput = theirFingerprint.toUtf8();
        QByteArray fpInfoBytes = (info + expectedKeyId).toUtf8();
        QByteArray computedFpMac(int(olm_sas_mac_length(sas)), '\0');
        olm_sas_calculate_mac_fixed_base64(sas, fpInput.constData(), fpInput.size(), fpInfoBytes.constData(), fpInfoBytes.size(), computedFpMac.data(), computedFpMac.size());

        if (QString::fromUtf8(computedFpMac) != macMap.value(expectedKeyId).toString()) {
            sendVerificationCancel("Device MAC doesn't match.", "m.key_mismatch");
            return;
        }

        m_verifiedDevices.insert(m_verification.theirUserId + "|" + m_verification.theirDeviceId);
        sendVerificationDone();
        setVerificationStatus("Device successfully verified!");
        setVerificationAwaitingConfirm(false);
        setVerificationSas(QString());
        setVerificationEmoji(QString());
        freeVerificationSas();
        m_verification = VerificationState();
        setVerificationIncoming(false, QString());
        return;
    }

    if (type == "m.key.verification.cancel") {
        setVerificationStatus("The other device cancelled: " + content.value("reason").toString());
        setVerificationAwaitingConfirm(false);
        setVerificationSas(QString());
        setVerificationEmoji(QString());
        freeVerificationSas();
        m_verification = VerificationState();
        setVerificationIncoming(false, QString());
        return;
    }

    if (type == "m.key.verification.done") {
        // We already declared success upon our own MAC check; nothing more to do.
        return;
    }
}
