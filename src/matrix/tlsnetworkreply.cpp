#include "tlsnetworkreply.hpp"

#ifdef BBPORT_HAVE_NATIVE_TLS

#include <string.h>
#include <stdio.h>

#include <QMutexLocker>
#include <QMetaObject>
#include <QVariant>
#include <QDebug>

#include "mbedtls/ssl.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/error.h"

// Temporary diagnostic logging (TLS transport bring-up) -- writes straight to
// a file instead of qWarning()/slog2, since qWarning() output wasn't showing
// up in slog2info in testing (build/config issue, unconfirmed which). Remove
// once the E2EE key-exchange regression is root-caused.
namespace {
QMutex g_logMutex;
void appendDebugLog(const QString &line)
{
    QMutexLocker locker(&g_logMutex);
    FILE *f = fopen("/accounts/1000/shared/misc/bbport_tls_log.txt", "a");
    if (!f) return;
    QByteArray utf8 = (line + "\n").toUtf8();
    fwrite(utf8.constData(), 1, utf8.size(), f);
    fclose(f);
}
}

// Shared, read-only-after-init CA store (parsing tools/cacert.pem, bundled
// as assets/cacert.pem, is not cheap and the result never changes). Each
// request thread still gets its own entropy/DRBG context -- mbedTLS's DRBG
// isn't thread-safe and reseeding per request is cheap -- but verifying a
// handshake against an already-parsed mbedtls_x509_crt chain from multiple
// threads concurrently is safe since verification never mutates it.
namespace {

struct TlsCaStore
{
    mbedtls_x509_crt cacert;
    bool ok;

    TlsCaStore() : ok(false)
    {
        mbedtls_x509_crt_init(&cacert);
        int ret = mbedtls_x509_crt_parse_file(&cacert, "app/native/assets/cacert.pem");
        ok = (ret >= 0);
    }
};

QMutex g_caStoreMutex;
TlsCaStore *g_caStore = 0;

TlsCaStore* caStore()
{
    QMutexLocker locker(&g_caStoreMutex);
    if (!g_caStore) {
        g_caStore = new TlsCaStore();
    }
    return g_caStore;
}

// Case-insensitive lookup into a parsed header list.
QByteArray headerValue(const QList<QPair<QByteArray, QByteArray> > &headers, const char *name)
{
    for (int i = 0; i < headers.size(); ++i) {
        if (qstricmp(headers.at(i).first.constData(), name) == 0) {
            return headers.at(i).second;
        }
    }
    return QByteArray();
}

// Reads whatever is available into `pending`, following mbedTLS's
// want-read/want-write retry contract. Returns the number of bytes
// appended, 0 on clean peer close, or a negative mbedtls error code.
int sslReadMore(mbedtls_ssl_context *ssl, QByteArray &pending, bool *peerClosed)
{
    unsigned char buf[4096];
    for (;;) {
        int n = mbedtls_ssl_read(ssl, buf, sizeof(buf));
        if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE) {
            continue;
        }
        if (n == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || n == 0) {
            *peerClosed = true;
            return 0;
        }
        if (n < 0) {
            return n;
        }
        pending.append(reinterpret_cast<const char *>(buf), n);
        return n;
    }
}

// Ensures `pending` holds at least `needed` bytes, reading more as needed.
bool ensureBytes(mbedtls_ssl_context *ssl, QByteArray &pending, int needed, bool *peerClosed)
{
    while (pending.size() < needed) {
        int n = sslReadMore(ssl, pending, peerClosed);
        if (n < 0 || *peerClosed) {
            return false;
        }
    }
    return true;
}

} // namespace

TlsRequestThread::TlsRequestThread(const QString &method, const QUrl &url,
                                    const QList<QPair<QByteArray, QByteArray> > &headers,
                                    const QByteArray &body, QObject *parent) :
        QThread(parent),
        httpStatus(0),
        netError(QNetworkReply::NoError),
        m_method(method),
        m_url(url),
        m_headers(headers),
        m_body(body),
        m_aborted(false)
{
    mbedtls_net_init(&m_netCtx);
}

TlsRequestThread::~TlsRequestThread()
{
    mbedtls_net_free(&m_netCtx);
}

void TlsRequestThread::requestAbort()
{
    QMutexLocker locker(&m_netMutex);
    m_aborted = true;
    mbedtls_net_free(&m_netCtx);
}

bool TlsRequestThread::wasAborted()
{
    QMutexLocker locker(&m_netMutex);
    return m_aborted;
}

void TlsRequestThread::run()
{
    TlsCaStore *ca = caStore();
    if (!ca->ok) {
        netError = QNetworkReply::UnknownContentError;
        netErrorString = "Couldn't load the CA certificate bundle (assets/cacert.pem).";
        return;
    }

    // Matrix media endpoints (thumbnail/download) commonly answer with a
    // 307 pointing at a CDN host; Qt's old QNetworkAccessManager backend
    // followed such redirects transparently, so this hand-rolled transport
    // has to do the same explicitly or media silently comes back empty.
    static const int kMaxRedirects = 5;
    QUrl currentUrl = m_url;

    for (int redirectCount = 0; ; ++redirectCount) {

    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context ctr_drbg;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&ctr_drbg);
    mbedtls_ssl_init(&ssl);
    mbedtls_ssl_config_init(&conf);

    char errbuf[256];
    bool failed = false;
    QByteArray redirectLocation;

    int ret = mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy,
                                     reinterpret_cast<const unsigned char *>("bbport"), 6);
    if (ret != 0) {
        netError = QNetworkReply::UnknownNetworkError;
        netErrorString = "RNG initialization failed.";
        failed = true;
    }

    QByteArray hostUtf8;
    QByteArray portAscii;
    QByteArray path = currentUrl.encodedPath();
    if (path.isEmpty()) path = "/";
    if (!currentUrl.encodedQuery().isEmpty()) {
        path += "?" + currentUrl.encodedQuery();
    }

    if (!failed) {
        hostUtf8 = currentUrl.host().toUtf8();
        int port = currentUrl.port(443);
        portAscii = QByteArray::number(port);

        {
            QMutexLocker netLocker(&m_netMutex);
            mbedtls_net_free(&m_netCtx);
            mbedtls_net_init(&m_netCtx);
        }

        ret = mbedtls_net_connect(&m_netCtx, hostUtf8.constData(), portAscii.constData(),
                                   MBEDTLS_NET_PROTO_TCP);
        if (ret != 0 || wasAborted()) {
            netError = wasAborted() ? QNetworkReply::OperationCanceledError
                                     : QNetworkReply::ConnectionRefusedError;
            if (!wasAborted()) {
                mbedtls_strerror(ret, errbuf, sizeof(errbuf));
                netErrorString = QString::fromLatin1(errbuf);
            }
            failed = true;
        }
    }

    if (!failed) {
        ret = mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_CLIENT,
                                           MBEDTLS_SSL_TRANSPORT_STREAM,
                                           MBEDTLS_SSL_PRESET_DEFAULT);
        if (ret != 0) {
            netError = QNetworkReply::SslHandshakeFailedError;
            netErrorString = "TLS configuration failed.";
            failed = true;
        }
    }

    if (!failed) {
        mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_REQUIRED);
        mbedtls_ssl_conf_ca_chain(&conf, &ca->cacert, 0);
        mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &ctr_drbg);

        ret = mbedtls_ssl_setup(&ssl, &conf);
        if (ret == 0) {
            QByteArray hostForSni = hostUtf8;
            ret = mbedtls_ssl_set_hostname(&ssl, hostForSni.constData());
        }
        if (ret != 0) {
            netError = QNetworkReply::SslHandshakeFailedError;
            netErrorString = "TLS setup failed.";
            failed = true;
        }
    }

    if (!failed) {
        mbedtls_ssl_set_bio(&ssl, &m_netCtx, mbedtls_net_send, mbedtls_net_recv, 0);

        while ((ret = mbedtls_ssl_handshake(&ssl)) != 0) {
            if (wasAborted()) {
                netError = QNetworkReply::OperationCanceledError;
                failed = true;
                break;
            }
            if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
                mbedtls_strerror(ret, errbuf, sizeof(errbuf));
                netError = QNetworkReply::SslHandshakeFailedError;
                netErrorString = QString::fromLatin1(errbuf);
                failed = true;
                break;
            }
        }
    }

    if (!failed) {
        uint32_t verifyFlags = mbedtls_ssl_get_verify_result(&ssl);
        if (verifyFlags != 0) {
            char vbuf[512];
            mbedtls_x509_crt_verify_info(vbuf, sizeof(vbuf), "", verifyFlags);
            netError = QNetworkReply::SslHandshakeFailedError;
            netErrorString = QString("Invalid certificate: %1").arg(QString::fromLatin1(vbuf).trimmed());
            failed = true;
        }
    }

    if (!failed) {
        // Matrix media redirects (307) point at a different host entirely
        // (S3/R2/Wasabi, authenticated purely via the presigned query
        // string) -- forwarding this request's own Authorization: Bearer
        // <homeserver access token> there is both a token leak to a
        // third-party host and, per real-world testing against Beeper's own
        // redirect targets, gets the request itself rejected outright.
        // Every other real HTTP client (curl, browsers, Qt's own
        // QNetworkAccessManager) drops Authorization on a cross-host
        // redirect by default for exactly the leak reason; this follows
        // suit and it also happens to fix the rejection.
        bool crossHost = redirectCount > 0 && !currentUrl.host().isEmpty() && currentUrl.host() != m_url.host();

        QByteArray request;
        request += m_method.toLatin1() + " " + path + " HTTP/1.1\r\n";
        request += "Host: " + hostUtf8 + "\r\n";
        for (int i = 0; i < m_headers.size(); ++i) {
            if (crossHost && qstricmp(m_headers.at(i).first.constData(), "Authorization") == 0) continue;
            request += m_headers.at(i).first + ": " + m_headers.at(i).second + "\r\n";
        }
        if (m_method == "POST" || m_method == "PUT" || !m_body.isEmpty()) {
            request += "Content-Length: " + QByteArray::number(m_body.size()) + "\r\n";
        }
        request += "Connection: close\r\n\r\n";
        request += m_body;

        int written = 0;
        while (written < request.size()) {
            ret = mbedtls_ssl_write(&ssl, reinterpret_cast<const unsigned char *>(request.constData()) + written,
                                     request.size() - written);
            if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
                continue;
            }
            if (ret < 0 || wasAborted()) {
                netError = wasAborted() ? QNetworkReply::OperationCanceledError
                                         : QNetworkReply::RemoteHostClosedError;
                failed = true;
                break;
            }
            written += ret;
        }
    }

    if (!failed) {
        QByteArray pending;
        bool peerClosed = false;
        int headerEnd = -1;

        for (;;) {
            headerEnd = pending.indexOf("\r\n\r\n");
            if (headerEnd >= 0) break;
            if (pending.size() > 1 * 1024 * 1024) {
                netError = QNetworkReply::ProtocolFailure;
                netErrorString = "HTTP headers too large.";
                failed = true;
                break;
            }
            int n = sslReadMore(&ssl, pending, &peerClosed);
            if (n < 0 || peerClosed) {
                netError = wasAborted() ? QNetworkReply::OperationCanceledError
                                         : QNetworkReply::RemoteHostClosedError;
                netErrorString = "Connection closed before HTTP headers.";
                failed = true;
                break;
            }
        }

        if (!failed) {
            QByteArray headerBlock = pending.left(headerEnd);
            pending.remove(0, headerEnd + 4);

            QList<QByteArray> lines = headerBlock.split('\n');
            QByteArray statusLine = lines.isEmpty() ? QByteArray() : lines.at(0).trimmed();
            QList<QByteArray> statusParts = statusLine.split(' ');
            bool statusOk = false;
            httpStatus = statusParts.size() >= 2 ? statusParts.at(1).toInt(&statusOk) : 0;
            if (!statusOk) {
                // A malformed status line used to silently become
                // httpStatus=0 (indistinguishable from "no status parsed
                // at all" in logs) instead of a reported protocol error.
                netError = QNetworkReply::ProtocolFailure;
                netErrorString = "Invalid HTTP status line.";
                failed = true;
            }

            QList<QPair<QByteArray, QByteArray> > respHeaders;
            for (int i = 1; i < lines.size(); ++i) {
                QByteArray line = lines.at(i).trimmed();
                int colon = line.indexOf(':');
                if (colon < 0) continue;
                respHeaders.append(qMakePair(line.left(colon).trimmed(), line.mid(colon + 1).trimmed()));
            }

            if (httpStatus >= 300 && httpStatus < 400) {
                redirectLocation = headerValue(respHeaders, "Location");
            }

            QByteArray te = headerValue(respHeaders, "Transfer-Encoding").toLower();
            QByteArray cl = headerValue(respHeaders, "Content-Length");

            QByteArray body;
            if (te.contains("chunked")) {
                for (;;) {
                    int lineEnd;
                    bool ioError = false;
                    while ((lineEnd = pending.indexOf("\r\n")) < 0) {
                        int n = sslReadMore(&ssl, pending, &peerClosed);
                        if (n < 0 || peerClosed) { ioError = true; break; }
                    }
                    if (ioError) { failed = true; break; }

                    bool sizeOk = false;
                    int chunkSize = pending.left(lineEnd).trimmed().toInt(&sizeOk, 16);
                    pending.remove(0, lineEnd + 2);
                    if (!sizeOk) { failed = true; break; }

                    if (chunkSize == 0) {
                        for (;;) {
                            int te2;
                            while ((te2 = pending.indexOf("\r\n")) < 0) {
                                int n = sslReadMore(&ssl, pending, &peerClosed);
                                if (n < 0 || peerClosed) { ioError = true; break; }
                            }
                            if (ioError) break;
                            QByteArray trailer = pending.left(te2);
                            pending.remove(0, te2 + 2);
                            if (trailer.isEmpty()) break;
                        }
                        break;
                    }

                    if (!ensureBytes(&ssl, pending, chunkSize + 2, &peerClosed)) { failed = true; break; }
                    body += pending.left(chunkSize);
                    pending.remove(0, chunkSize + 2);
                }
                if (failed) {
                    netError = QNetworkReply::RemoteHostClosedError;
                    netErrorString = "Connection interrupted during chunked body.";
                }
            } else if (!cl.isEmpty()) {
                bool lenOk = false;
                int len = cl.trimmed().toInt(&lenOk);
                if (!lenOk || len < 0) {
                    // A non-numeric or out-of-int-range Content-Length used
                    // to silently become len=0 (empty body reported as a
                    // successful response) instead of the protocol error it
                    // actually is.
                    netError = QNetworkReply::ProtocolFailure;
                    netErrorString = "Invalid Content-Length.";
                    failed = true;
                } else if (!ensureBytes(&ssl, pending, len, &peerClosed)) {
                    netError = QNetworkReply::RemoteHostClosedError;
                    netErrorString = "Connection interrupted before end of body.";
                    failed = true;
                } else {
                    body = pending.left(len);
                }
            } else {
                body = pending;
                bool readError = false;
                for (;;) {
                    int n = sslReadMore(&ssl, body, &peerClosed);
                    if (peerClosed) break; // expected/correct end of a read-until-close body
                    if (n < 0) { readError = true; break; }
                }
                if (readError) {
                    // A genuine mbedtls read error (reset connection, TLS
                    // alert mid-stream, ...) previously fell through this
                    // same "|| n <= 0" break as a clean close, silently
                    // returning a truncated body marked as success.
                    netError = QNetworkReply::RemoteHostClosedError;
                    netErrorString = "Connection interrupted while reading body.";
                    failed = true;
                }
            }

            if (!failed) {
                responseBody = body;
            }
        }
    }

    mbedtls_ssl_close_notify(&ssl);
    mbedtls_ssl_free(&ssl);
    mbedtls_ssl_config_free(&conf);
    mbedtls_ctr_drbg_free(&ctr_drbg);
    mbedtls_entropy_free(&entropy);

    // Temporary diagnostic logging (TLS transport bring-up) -- remove once
    // the E2EE key-exchange regression is root-caused. Never logs the
    // request/response body for /sync (message content) or the
    // Authorization header; crypto-handshake endpoints get a body preview
    // since their payload is just device-key JSON, not chat content.
    // Built via plain concatenation, not QString::arg() chaining: these
    // values (URLs/paths especially) routinely contain literal "%2F"-style
    // percent-encoding, and a later .arg() call in a chain will happily
    // treat a stray "%2" inside an already-substituted value as its own
    // placeholder and overwrite it -- confirmed on-device turning
    // "...ND%2F2026..." into "...ND200F2026..." (the status code eating the
    // escaped slash). QString::arg() itself is fine; chaining several
    // calls after inserting untrusted/dynamic text is not.
    bool isCryptoEndpoint = path.contains("/keys/") || path.contains("/sendToDevice/");
    appendDebugLog(m_method + " " + QString::fromUtf8(path) +
                   " status=" + QString::number(httpStatus) +
                   " bodyBytes=" + QString::number(responseBody.size()) +
                   " netError=" + QString::number(int(netError)) +
                   " " + netErrorString);
    if (isCryptoEndpoint) {
        appendDebugLog("  reqBody=" + QString::fromUtf8(m_body.left(500)));
        appendDebugLog("  respBody=" + QString::fromUtf8(responseBody.left(500)));
    } else if (httpStatus >= 400) {
        // Non-crypto endpoints normally never log bodies (chat content), but
        // a 4xx/5xx error body is never message content -- for media
        // downloads specifically this is what should have the S3/R2
        // rejection reason (SignatureDoesNotMatch, AccessDenied, etc.).
        appendDebugLog("  errRespBody=" + QString::fromUtf8(responseBody.left(1000)));
    }

    if (!failed && !redirectLocation.isEmpty() && redirectCount < kMaxRedirects) {
        QUrl redirectUrl = currentUrl.resolved(QUrl::fromEncoded(redirectLocation));
        appendDebugLog("  following redirect status=" + QString::number(httpStatus) +
                       " -> " + redirectUrl.toString());
        currentUrl = redirectUrl;
        continue;
    }

    break;
    } // for redirectCount
}

TlsNetworkReply::TlsNetworkReply(QNetworkAccessManager::Operation op, const QNetworkRequest &request,
                                  const QByteArray &outgoingData, QObject *parent) :
        QNetworkReply(parent),
        m_worker(0),
        m_readPos(0)
{
    setOperation(op);
    setRequest(request);
    setUrl(request.url());
    setOpenMode(QIODevice::ReadOnly);

    QList<QPair<QByteArray, QByteArray> > headers;
    QList<QByteArray> rawNames = request.rawHeaderList();
    for (int i = 0; i < rawNames.size(); ++i) {
        headers.append(qMakePair(rawNames.at(i), request.rawHeader(rawNames.at(i))));
    }
    QVariant contentType = request.header(QNetworkRequest::ContentTypeHeader);
    if (contentType.isValid()) {
        headers.append(qMakePair(QByteArray("Content-Type"), contentType.toString().toUtf8()));
    }

    QString method = "GET";
    switch (op) {
        case QNetworkAccessManager::GetOperation: method = "GET"; break;
        case QNetworkAccessManager::PostOperation: method = "POST"; break;
        case QNetworkAccessManager::PutOperation: method = "PUT"; break;
        case QNetworkAccessManager::DeleteOperation: method = "DELETE"; break;
        case QNetworkAccessManager::HeadOperation: method = "HEAD"; break;
        default: method = "GET"; break;
    }

    m_worker = new TlsRequestThread(method, request.url(), headers, outgoingData, this);
    connect(m_worker, SIGNAL(finished()), this, SLOT(onWorkerFinished()));
    m_worker->start();
}

TlsNetworkReply::~TlsNetworkReply()
{
    if (m_worker) {
        m_worker->requestAbort();
        m_worker->wait();
    }
}

void TlsNetworkReply::abort()
{
    if (m_worker) {
        m_worker->requestAbort();
    }
}

qint64 TlsNetworkReply::bytesAvailable() const
{
    return (m_buffer.size() - m_readPos) + QNetworkReply::bytesAvailable();
}

qint64 TlsNetworkReply::readData(char *data, qint64 maxlen)
{
    qint64 available = m_buffer.size() - m_readPos;
    if (available <= 0) {
        return isFinished() ? -1 : 0;
    }
    qint64 toCopy = qMin(maxlen, available);
    memcpy(data, m_buffer.constData() + m_readPos, toCopy);
    m_readPos += toCopy;
    return toCopy;
}

void TlsNetworkReply::onWorkerFinished()
{
    setError(m_worker->netError, m_worker->netErrorString);
    setAttribute(QNetworkRequest::HttpStatusCodeAttribute, m_worker->httpStatus);
    m_buffer = m_worker->responseBody;

    setFinished(true);
    emit downloadProgress(m_buffer.size(), m_buffer.size());
    emit finished();
    if (m_worker->netError != QNetworkReply::NoError) {
        emit error(m_worker->netError);
    }

    m_worker->deleteLater();
    m_worker = 0;
}

#endif /* BBPORT_HAVE_NATIVE_TLS */
