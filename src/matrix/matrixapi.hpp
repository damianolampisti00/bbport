#ifndef MATRIXAPI_HPP_
#define MATRIXAPI_HPP_

#include <QObject>
#include <QString>
#include <QVariant>
#include <QNetworkAccessManager>

class QNetworkReply;

// Low-level Matrix client-server API transport: builds authenticated HTTP
// requests against a homeserver and exposes helpers to parse JSON replies.
// Higher level pieces (SyncEngine, MediaManager, models) own the QNetworkReply
// objects they get back and parse them with MatrixApi::parseJson().
class MatrixApi : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool loggedIn READ isLoggedIn NOTIFY loggedInChanged)
    Q_PROPERTY(QString homeserver READ homeserver NOTIFY loggedInChanged)
    Q_PROPERTY(QString userId READ userId NOTIFY loggedInChanged)
    Q_PROPERTY(bool busy READ isBusy NOTIFY busyChanged)
    Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged)

public:
    explicit MatrixApi(QObject *parent = 0);
    virtual ~MatrixApi();

    bool isLoggedIn() const;
    bool isBusy() const;
    QString homeserver() const;
    QString userId() const;
    QString deviceId() const;
    QString accessToken() const;
    QString lastError() const;

    // Sets the device_id to request at the next password login, so the
    // server reuses the same device across app runs (see OlmCryptoManager,
    // which needs a stable device identity for its persisted Olm account to
    // mean anything to other devices). Call before loginWithPassword().
    void setPreferredDeviceId(const QString &deviceId);

    // Builds "<homeserver>/_matrix/client/r0<path>" with the given query
    // string items appended, and issues an authenticated request.
    QNetworkReply* apiGet(const QString &path, const QVariantMap &query = QVariantMap());
    QNetworkReply* apiPost(const QString &path, const QVariant &jsonBody);
    QNetworkReply* apiPut(const QString &path, const QVariant &jsonBody);
    // Raw body upload (used for media), bypasses JSON encoding.
    QNetworkReply* apiPostRaw(const QString &path, const QByteArray &body, const QString &contentType);

    // Absolute-URL GET, used for mxc:// media download/thumbnail links which
    // already contain the full "/_matrix/media/..." path.
    QNetworkReply* rawGet(const QString &absolutePath, const QVariantMap &query = QVariantMap());

    QNetworkAccessManager* networkManager();

    // Monotonically increasing transaction id for idempotent sends.
    Q_INVOKABLE QString nextTxnId();

    // Parses a finished QNetworkReply body as JSON. Returns an invalid
    // QVariant and sets *ok=false on transport or JSON errors.
    static QVariant parseJson(QNetworkReply *reply, bool *ok);

public slots:
    void loginWithPassword(const QString &homeserver, const QString &username, const QString &password);
    void loginWithToken(const QString &homeserver, const QString &userId, const QString &accessToken);
    void logout();

signals:
    void loginSucceeded();
    void loginFailed(const QString &error);
    void loggedInChanged();
    void busyChanged();
    void lastErrorChanged();

private slots:
    void onLoginReplyFinished();
    void onWhoAmIReplyFinished();

private:
    static QString normalizeHomeserver(const QString &homeserver);
    QUrl buildUrl(const QString &basePath, const QString &path, const QVariantMap &query) const;
    QNetworkRequest authorizedRequest(const QUrl &url, const QString &contentType = QString()) const;
    void setBusy(bool busy);
    void setLastError(const QString &error);

    QNetworkAccessManager *m_nam;
    QString m_homeserver;
    QString m_accessToken;
    QString m_userId;
    QString m_deviceId;
    QString m_preferredDeviceId;
    QString m_lastError;
    qint64 m_txnCounter;
    bool m_busy;
};

#endif /* MATRIXAPI_HPP_ */
