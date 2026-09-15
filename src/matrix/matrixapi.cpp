#include "matrixapi.hpp"

#include <bb/data/JsonDataAccess>

#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>
#include <QDateTime>
#include <QDir>
#include <QFile>

#ifdef BBPORT_HAVE_NATIVE_TLS
#include "tlsnetworkaccessmanager.hpp"
#endif

using namespace bb::data;

static const char *kClientApiBase = "/_matrix/client/r0";
static const char *kUserAgent = "BBport/1.0 (BlackBerry 10)";

// Private to this app (QDir::homePath(), same directory SyncEngine already
// uses for sync_since_token.txt/room_cache.json) -- both the foreground UI
// binary and the headless background binary share this same sandbox (same
// app id, just two invoke-target entry points in one .bar), so whichever one
// runs first writes it and either can read it back.
static QString sessionFilePath()
{
    return QDir::homePath() + "/session.json";
}

MatrixApi::MatrixApi(QObject *parent) :
        QObject(parent),
#ifdef BBPORT_HAVE_NATIVE_TLS
        // Device builds do TLS 1.2 natively via mbedTLS (BB10's system
        // OpenSSL only speaks TLS 1.0, which real homeservers reject) --
        // see tlsnetworkreply.hpp. Simulator builds fall back to Qt's own
        // QSslSocket-backed QNetworkAccessManager, which can't reach a real
        // homeserver directly (see the Simulator note in README.md) --
        // untested/unsupported for actual login, kept only so the
        // Simulator-Debug configuration still compiles.
        m_nam(new TlsNetworkAccessManager(this)),
#else
        m_nam(new QNetworkAccessManager(this)),
#endif
        m_txnCounter(0),
        m_busy(false),
        m_hasSavedSession(QFile::exists(sessionFilePath()))
{
}

MatrixApi::~MatrixApi()
{
}

bool MatrixApi::isLoggedIn() const
{
    return !m_accessToken.isEmpty();
}

bool MatrixApi::isBusy() const
{
    return m_busy;
}

bool MatrixApi::hasSavedSession() const
{
    return m_hasSavedSession;
}

QString MatrixApi::homeserver() const
{
    return m_homeserver;
}

QString MatrixApi::userId() const
{
    return m_userId;
}

QString MatrixApi::deviceId() const
{
    return m_deviceId;
}

QString MatrixApi::accessToken() const
{
    return m_accessToken;
}

QString MatrixApi::lastError() const
{
    return m_lastError;
}

void MatrixApi::setPreferredDeviceId(const QString &deviceId)
{
    m_preferredDeviceId = deviceId;
}

void MatrixApi::setBusy(bool busy)
{
    if (m_busy == busy) return;
    m_busy = busy;
    emit busyChanged();
}

void MatrixApi::setLastError(const QString &error)
{
    m_lastError = error;
    emit lastErrorChanged();
}

QString MatrixApi::nextTxnId()
{
    m_txnCounter++;
    return QString("bbport-%1-%2").arg(QDateTime::currentMSecsSinceEpoch()).arg(m_txnCounter);
}

QUrl MatrixApi::buildUrl(const QString &basePath, const QString &path, const QVariantMap &query) const
{
    // Callers pre-encode dynamic path segments themselves (room IDs, event
    // IDs, ... contain '!'/':'/'$' that need escaping). The plain QUrl(QString)
    // constructor parses in TolerantMode, which can re-escape those already-
    // encoded '%XX' sequences (turning "%21" into "%2521"). fromEncoded()
    // takes the string as already-fully-encoded bytes and leaves it alone.
    QUrl url = QUrl::fromEncoded((m_homeserver + basePath + path).toUtf8());
    QMapIterator<QString, QVariant> it(query);
    while (it.hasNext()) {
        it.next();
        url.addQueryItem(it.key(), it.value().toString());
    }
    return url;
}

QNetworkRequest MatrixApi::authorizedRequest(const QUrl &url, const QString &contentType) const
{
    QNetworkRequest request(url);
    request.setRawHeader("User-Agent", kUserAgent);
    if (!m_accessToken.isEmpty()) {
        request.setRawHeader("Authorization", QString("Bearer %1").arg(m_accessToken).toUtf8());
    }
    if (!contentType.isEmpty()) {
        request.setHeader(QNetworkRequest::ContentTypeHeader, contentType);
    }
    return request;
}

QNetworkReply* MatrixApi::apiGet(const QString &path, const QVariantMap &query)
{
    QUrl url = buildUrl(kClientApiBase, path, query);
    return m_nam->get(authorizedRequest(url));
}

QNetworkReply* MatrixApi::rawGet(const QString &absolutePath, const QVariantMap &query)
{
    QUrl url = buildUrl(QString(), absolutePath, query);
    return m_nam->get(authorizedRequest(url));
}

QNetworkReply* MatrixApi::apiPost(const QString &path, const QVariant &jsonBody)
{
    QUrl url = buildUrl(kClientApiBase, path, QVariantMap());
    QByteArray body;
    JsonDataAccess jda;
    jda.saveToBuffer(jsonBody, &body);
    QNetworkRequest request = authorizedRequest(url, "application/json");
    return m_nam->post(request, body);
}

QNetworkReply* MatrixApi::apiPut(const QString &path, const QVariant &jsonBody)
{
    QUrl url = buildUrl(kClientApiBase, path, QVariantMap());
    QByteArray body;
    JsonDataAccess jda;
    jda.saveToBuffer(jsonBody, &body);
    QNetworkRequest request = authorizedRequest(url, "application/json");
    return m_nam->put(request, body);
}

QNetworkReply* MatrixApi::apiPostRaw(const QString &path, const QByteArray &body, const QString &contentType)
{
    QUrl url = buildUrl(QString(), path, QVariantMap());
    QNetworkRequest request = authorizedRequest(url, contentType);
    return m_nam->post(request, body);
}

QVariant MatrixApi::parseJson(QNetworkReply *reply, bool *ok)
{
    if (ok) *ok = false;
    if (!reply) return QVariant();

    QByteArray data = reply->readAll();
    if (reply->error() != QNetworkReply::NoError && data.isEmpty()) {
        return QVariant();
    }

    JsonDataAccess jda;
    QVariant result = jda.loadFromBuffer(data);
    if (jda.hasError()) {
        return QVariant();
    }
    if (ok) *ok = true;
    return result;
}

QString MatrixApi::normalizeHomeserver(const QString &homeserver)
{
    QString result = homeserver.trimmed();
    if (!result.startsWith("http://", Qt::CaseInsensitive) && !result.startsWith("https://", Qt::CaseInsensitive)) {
        result.prepend("https://");
    }
    while (result.endsWith("/")) result.chop(1);
    return result;
}

void MatrixApi::loginWithPassword(const QString &homeserver, const QString &username, const QString &password)
{
    m_homeserver = normalizeHomeserver(homeserver);
    setBusy(true);
    setLastError(QString());

    QVariantMap body;
    body["type"] = "m.login.password";
    body["user"] = username;
    body["password"] = password;
    body["initial_device_display_name"] = "BBport BlackBerry 10";
    if (!m_preferredDeviceId.isEmpty()) body["device_id"] = m_preferredDeviceId;

    QNetworkReply *reply = apiPost("/login", body);
    connect(reply, SIGNAL(finished()), this, SLOT(onLoginReplyFinished()));
}

void MatrixApi::onLoginReplyFinished()
{
    QNetworkReply *reply = qobject_cast<QNetworkReply*>(sender());
    setBusy(false);
    if (!reply) return;

    QNetworkReply::NetworkError netError = reply->error();
    QString netErrorString = reply->errorString();

    bool ok = false;
    QVariant parsed = parseJson(reply, &ok);
    QVariantMap map = parsed.toMap();
    reply->deleteLater();

    if (!ok || !map.contains("access_token")) {
        QString err;
        if (map.contains("error")) {
            err = QString("%1 (%2)").arg(map.value("error").toString(), map.value("errcode").toString());
        } else if (netError != QNetworkReply::NoError) {
            err = QString("Network error: %1").arg(netErrorString);
        } else {
            err = "Invalid response from server.";
        }
        setLastError(err);
        emit loginFailed(err);
        return;
    }

    m_accessToken = map.value("access_token").toString();
    m_userId = map.value("user_id").toString();
    m_deviceId = map.value("device_id").toString();
    saveSession();
    emit loggedInChanged();
    emit loginSucceeded();
}

void MatrixApi::loginWithToken(const QString &homeserver, const QString &userId, const QString &accessToken)
{
    m_homeserver = normalizeHomeserver(homeserver);
    m_accessToken = accessToken;
    m_userId = userId;
    setBusy(true);
    setLastError(QString());

    // Validate the supplied token before declaring success.
    QNetworkReply *reply = apiGet("/account/whoami");
    connect(reply, SIGNAL(finished()), this, SLOT(onWhoAmIReplyFinished()));
}

void MatrixApi::onWhoAmIReplyFinished()
{
    QNetworkReply *reply = qobject_cast<QNetworkReply*>(sender());
    setBusy(false);
    if (!reply) return;

    QNetworkReply::NetworkError netError = reply->error();
    QString netErrorString = reply->errorString();

    bool ok = false;
    QVariant parsed = parseJson(reply, &ok);
    QVariantMap map = parsed.toMap();
    reply->deleteLater();

    if (!ok || !map.contains("user_id")) {
        m_accessToken.clear();
        m_userId.clear();
        QString err;
        if (map.contains("error")) {
            err = QString("%1 (%2)").arg(map.value("error").toString(), map.value("errcode").toString());
        } else if (netError != QNetworkReply::NoError) {
            err = QString("Network error: %1").arg(netErrorString);
        } else {
            err = "Invalid token or unexpected response from server.";
        }
        setLastError(err);
        emit loginFailed(err);
        return;
    }

    m_userId = map.value("user_id").toString();
    // /account/whoami's device_id is optional per spec (added after the
    // base whoami response), but when present it's the actual device this
    // pre-existing access token is bound to server-side -- unlike
    // loginWithPassword()'s /login response, there's no way to request a
    // specific device_id here since the token/device pairing was already
    // fixed before this app ever saw it. Previously left m_deviceId empty
    // for this entire login path.
    if (map.contains("device_id")) {
        m_deviceId = map.value("device_id").toString();
    }
    saveSession();
    emit loggedInChanged();
    emit loginSucceeded();
}

void MatrixApi::tryAutoLogin()
{
    if (!m_hasSavedSession) return;

    QFile file(sessionFilePath());
    if (!file.open(QIODevice::ReadOnly)) return;
    QByteArray buffer = file.readAll();
    file.close();

    JsonDataAccess jda;
    QVariant parsed = jda.loadFromBuffer(buffer);
    if (jda.hasError()) return;

    QVariantMap session = parsed.toMap();
    QString homeserver = session.value("homeserver").toString();
    QString userId = session.value("userId").toString();
    QString accessToken = session.value("accessToken").toString();
    if (homeserver.isEmpty() || userId.isEmpty() || accessToken.isEmpty()) return;

    // loginWithToken() itself doesn't touch m_deviceId -- onWhoAmIReplyFinished()
    // only overwrites it when the server's response actually includes one
    // (device_id is optional per spec there). Pre-filling it from the saved
    // session here means saveSession() re-persists the already-known value
    // instead of silently blanking it out on a restore where the server
    // happens to omit it. (OlmCryptoManager's own device identity is
    // unrelated -- it persists its deviceId itself in bbport_olm/account.dat,
    // independent of anything MatrixApi does.)
    m_deviceId = session.value("deviceId").toString();
    loginWithToken(homeserver, userId, accessToken);
}

void MatrixApi::saveSession() const
{
    QVariantMap session;
    session["homeserver"] = m_homeserver;
    session["userId"] = m_userId;
    session["accessToken"] = m_accessToken;
    session["deviceId"] = m_deviceId;

    JsonDataAccess jda;
    QByteArray buffer;
    jda.saveToBuffer(QVariant(session), &buffer);
    QFile file(sessionFilePath());
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        file.write(buffer);
    }
}

void MatrixApi::clearSession() const
{
    QFile::remove(sessionFilePath());
}

void MatrixApi::logout()
{
    if (m_accessToken.isEmpty()) return;
    QNetworkReply *reply = apiPost("/logout", QVariantMap());
    connect(reply, SIGNAL(finished()), reply, SLOT(deleteLater()));
    m_accessToken.clear();
    m_userId.clear();
    m_deviceId.clear();
    m_homeserver.clear();
    clearSession();
    emit loggedInChanged();
}
