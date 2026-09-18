#include "tlsnetworkreply.hpp"
#include "bbportlog.hpp"

#ifdef BBPORT_HAVE_NATIVE_TLS

#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>

#include <QMutexLocker>
#include <QMetaObject>
#include <QVariant>
#include <QDebug>
#include <QAtomicInt>
#include <QHash>

#include "mbedtls/ssl.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/error.h"

namespace {

// Live thread/reply counts (see conversation): pinpoint which requests'
// TlsRequestThread/TlsNetworkReply pair is never destroyed. Every create/
// destroy logs the live count and the URL via bbportLog(). If the live
// counts ever climb without bound again, whichever URL shows a "+thread"/
// "+reply" with no matching "-thread"/"-reply" by the time it crashes is
// the leak; if instead every create is matched by a destroy but the count
// is still huge at once, it's an unbounded-concurrency burst (e.g. an
// unbatched key-query fan-out) rather than a leak. Cheap enough (one
// atomic op alongside a log line already being written for other reasons)
// to leave in permanently as a standing diagnostic, unlike the separate
// bbport_tls_log.txt file this used to also write on every single request/
// response -- that one was genuinely temporary bring-up logging and has
// been removed now that the regression it was tracking is long fixed.
QAtomicInt g_liveThreads(0);
QAtomicInt g_liveReplies(0);
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

// TLS session cache, keyed by host, so a repeat connection to the same host
// (overwhelmingly the homeserver itself: every ~30s /sync long-poll plus
// whatever else runs alongside it) can do mbedTLS's much cheaper abbreviated
// resumption handshake instead of a full one. A full TLS handshake means a
// fresh asymmetric key exchange (ECDHE) and certificate chain verification
// every single time -- by far the most expensive thing this app does on a
// ~1.2GHz Cortex-A9 with no crypto acceleration, and it was happening on a
// strict, unavoidable ~30s cycle for as long as the app stayed open,
// regardless of whether anything had actually changed. Resumption is purely
// additive/safe: mbedTLS falls back to a full handshake on its own if the
// server doesn't recognize/accept the offered session (expired, evicted,
// server restarted, ...), so there's no failure mode here worse than "no
// speedup this time." Sessions are copied in and out via
// mbedtls_ssl_get_session()/set_session() (both documented as deep,
// independent copies -- see their own doc comments in mbedtls/ssl.h), so
// the cached copy here is safe to reuse across many concurrent
// TlsRequestThreads without any of them able to mutate what another is
// mid-read of.
QMutex g_sessionCacheMutex;
QHash<QString, mbedtls_ssl_session*> g_sessionCache;

// Called right after mbedtls_ssl_setup() succeeds, before the handshake --
// a no-op (mbedTLS just proceeds with a full handshake as usual) if this
// host has never been resumed before.
void tryResumeSession(mbedtls_ssl_context *ssl, const QString &host)
{
    bool attempted = false;
    {
        // Held across the set_session() call itself, not just the lookup:
        // set_session() reads from `cached` synchronously to make its own
        // copy, so it must not race a concurrent cacheSessionFor() call for
        // the same host freeing that exact pointer out from under it.
        QMutexLocker locker(&g_sessionCacheMutex);
        mbedtls_ssl_session *cached = g_sessionCache.value(host, 0);
        if (cached) {
            mbedtls_ssl_set_session(ssl, cached);
            attempted = true;
        }
    }
    if (attempted) bbportLog("[BBport:tls] attempting session resumption for " + host);
}

// Called after a successful, verified handshake -- replaces whatever was
// previously cached for this host (an existing entry is already stale the
// moment a new full/resumed handshake just completed) so the next request
// to the same host gets the freshest ticket.
void cacheSessionFor(mbedtls_ssl_context *ssl, const QString &host)
{
    mbedtls_ssl_session *session = new mbedtls_ssl_session();
    mbedtls_ssl_session_init(session);
    if (mbedtls_ssl_get_session(ssl, session) != 0) {
        mbedtls_ssl_session_free(session);
        delete session;
        return;
    }

    QMutexLocker locker(&g_sessionCacheMutex);
    mbedtls_ssl_session *old = g_sessionCache.value(host, 0);
    g_sessionCache[host] = session;
    locker.unlock();
    if (old) {
        mbedtls_ssl_session_free(old);
        delete old;
    }
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
// appended, 0 on clean peer close (or on abort -- see below), or a negative
// mbedtls error code.
//
// requestAbort() (called from another thread) is documented as unblocking a
// pending read by shutdown()'ing the fd, but that alone isn't reliable
// enough: a /sync long-poll sitting in this call is exactly the case most
// likely to be mid-read when the watchdog times out, and if that particular
// shutdown() doesn't actually interrupt the underlying blocking MsgReceive,
// this loop retried forever and the thread never returned from run() --
// leaked permanently (root cause behind a Momentics debug session showing
// 226 live threads and eventual bps_channel_create/bps_initialize failures
// -- see conversation). Fixed by not trusting the cross-thread nudge alone:
// run() configures mbedTLS's own read-timeout receive path (see its
// mbedtls_ssl_conf_read_timeout()/mbedtls_net_recv_timeout() comment), so a
// stalled read comes back MBEDTLS_ERR_SSL_TIMEOUT on its own on a bounded
// cadence, and this thread checks its own abort flag right here before ever
// retrying -- WANT_READ/WANT_WRITE can still happen too (e.g. a TLS
// renegotiation needing a write) and get the same treatment.
int sslReadMore(mbedtls_ssl_context *ssl, QByteArray &pending, bool *peerClosed, TlsRequestThread *self)
{
    unsigned char buf[4096];
    for (;;) {
        int n = mbedtls_ssl_read(ssl, buf, sizeof(buf));
        if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE || n == MBEDTLS_ERR_SSL_TIMEOUT) {
            if (self->wasAborted()) {
                *peerClosed = true;
                return 0;
            }
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
bool ensureBytes(mbedtls_ssl_context *ssl, QByteArray &pending, int needed, bool *peerClosed, TlsRequestThread *self)
{
    while (pending.size() < needed) {
        int n = sslReadMore(ssl, pending, peerClosed, self);
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
    int n = g_liveThreads.fetchAndAddRelaxed(1) + 1;
    bbportLog(QString("[BBport:tls] +thread %1 %2 live=%3").arg(m_method, m_url.toString()).arg(n));
}

TlsRequestThread::~TlsRequestThread()
{
    int n = g_liveThreads.fetchAndAddRelaxed(-1) - 1;
    bbportLog(QString("[BBport:tls] -thread %1 live=%2").arg(m_url.toString()).arg(n));
    mbedtls_net_free(&m_netCtx);
}

void TlsRequestThread::requestAbort()
{
    QMutexLocker locker(&m_netMutex);
    m_aborted = true;
    // shutdown(), not mbedtls_net_free(): this can run on another thread
    // concurrently with run() blocked inside mbedtls_ssl_handshake/read/
    // write, which invoke mbedTLS's own net_recv/net_send callbacks
    // directly on m_netCtx.fd -- those aren't guarded by m_netMutex (mbedTLS
    // has no notion of it), so freeing/closing the fd here raced whatever
    // blocking syscall was in flight on it. Worse, since several
    // TlsRequestThreads run concurrently (the /sync long-poll plus any API
    // call or media fetch), the instant close() releases this fd number the
    // kernel is free to hand it to a brand-new socket() from a *different*,
    // still-running request thread -- the aborted thread's still-blocked
    // read could then silently consume bytes belonging to that unrelated
    // connection. shutdown() interrupts the blocked call immediately
    // (it returns an error/EOF) without releasing the fd number itself; the
    // actual close happens later, once run() has genuinely returned, via
    // mbedtls_net_free() in the destructor -- safe by then since
    // TlsNetworkReply always wait()s for this thread to finish before it
    // can be destroyed.
    if (m_netCtx.fd >= 0) {
        shutdown(m_netCtx.fd, SHUT_RDWR);
    }
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
        // Bounds every individual blocking read below (handshake, HTTP
        // response, and in particular a /sync long-poll) to a few seconds at
        // a time via mbedTLS's own timeout-aware receive path (see the
        // mbedtls_ssl_set_bio() call just below), so a stalled read comes
        // back MBEDTLS_ERR_SSL_TIMEOUT on its own instead of blocking
        // indefinitely -- see sslReadMore()'s comment for why this thread
        // can no longer rely solely on requestAbort()'s cross-thread
        // shutdown() to notice an abort.
        //
        // A raw socket-level SO_RCVTIMEO was tried here first and broke
        // every /sync long-poll outright (every plain request -- login,
        // keys/upload, ... -- still succeeded, since those get an answer in
        // well under this timeout, but /sync's up-to-30s wait hit it on
        // every single attempt): confirmed on-device that
        // mbedtls_net_recv() (the plain, non-timeout-aware recv callback)
        // doesn't reliably turn a SO_RCVTIMEO expiry on this QNX network
        // stack into MBEDTLS_ERR_SSL_WANT_READ, so mbedTLS treated the
        // *expected*, ordinary "nothing happened yet" case of an idle long-
        // poll as a hard read failure and tore the connection down. mbedTLS
        // ships mbedtls_net_recv_timeout() plus mbedtls_ssl_conf_read_timeout()
        // for exactly this scenario -- it already returns the distinct,
        // properly-mbedTLS-recognized MBEDTLS_ERR_SSL_TIMEOUT on its own
        // timeout, with no dependency on how this platform's recv()
        // reports EAGAIN/EWOULDBLOCK/ETIMEDOUT.
        mbedtls_ssl_conf_read_timeout(&conf, 5000);

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
        mbedtls_ssl_set_bio(&ssl, &m_netCtx, mbedtls_net_send, mbedtls_net_recv, mbedtls_net_recv_timeout);
        tryResumeSession(&ssl, QString::fromUtf8(hostUtf8));

        while ((ret = mbedtls_ssl_handshake(&ssl)) != 0) {
            if (wasAborted()) {
                netError = QNetworkReply::OperationCanceledError;
                failed = true;
                break;
            }
            if (ret == MBEDTLS_ERR_SSL_TIMEOUT) continue; // no data within this read's slice -- not a failure, keep handshaking
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
        } else {
            // Verified (full handshake) or carried over from a prior
            // verified handshake (resumed -- see mbedtls_ssl_get_verify_result()'s
            // own doc comment on why that's still trustworthy here) alike:
            // cache it so the *next* request to this host can attempt
            // resumption too.
            cacheSessionFor(&ssl, QString::fromUtf8(hostUtf8));
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
            int n = sslReadMore(&ssl, pending, &peerClosed, this);
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
                        int n = sslReadMore(&ssl, pending, &peerClosed, this);
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
                                int n = sslReadMore(&ssl, pending, &peerClosed, this);
                                if (n < 0 || peerClosed) { ioError = true; break; }
                            }
                            if (ioError) break;
                            QByteArray trailer = pending.left(te2);
                            pending.remove(0, te2 + 2);
                            if (trailer.isEmpty()) break;
                        }
                        break;
                    }

                    if (!ensureBytes(&ssl, pending, chunkSize + 2, &peerClosed, this)) { failed = true; break; }
                    body += pending.left(chunkSize);
                    pending.remove(0, chunkSize + 2);
                }
                if (failed) {
                    netError = wasAborted() ? QNetworkReply::OperationCanceledError
                                             : QNetworkReply::RemoteHostClosedError;
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
                } else if (!ensureBytes(&ssl, pending, len, &peerClosed, this)) {
                    netError = wasAborted() ? QNetworkReply::OperationCanceledError
                                             : QNetworkReply::RemoteHostClosedError;
                    netErrorString = "Connection interrupted before end of body.";
                    failed = true;
                } else {
                    body = pending.left(len);
                }
            } else {
                body = pending;
                bool readError = false;
                for (;;) {
                    int n = sslReadMore(&ssl, body, &peerClosed, this);
                    if (peerClosed) {
                        // sslReadMore() also reports an abort through
                        // peerClosed (see its comment) -- treat that as a
                        // real failure, not a clean end of body, or an
                        // aborted request would silently "succeed" with a
                        // truncated body instead of reporting cancellation.
                        if (wasAborted()) readError = true;
                        break;
                    }
                    if (n < 0) { readError = true; break; }
                }
                if (readError) {
                    // A genuine mbedtls read error (reset connection, TLS
                    // alert mid-stream, ...) previously fell through this
                    // same "|| n <= 0" break as a clean close, silently
                    // returning a truncated body marked as success.
                    netError = wasAborted() ? QNetworkReply::OperationCanceledError
                                             : QNetworkReply::RemoteHostClosedError;
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

    // Per-request outcome is already visible via bbportLog() at the
    // TlsNetworkReply layer (+reply/-reply/worker finished, with URL/status/
    // error) -- this used to ALSO write a full second copy of every
    // request/response (including crypto-endpoint body previews) to a
    // separate bbport_tls_log.txt file for a since-fixed E2EE regression.
    // That doubled the disk I/O of every single TLS request/long-poll
    // forever, for a debugging need that no longer exists; removed rather
    // than left "temporarily" running indefinitely.
    if (!failed && !redirectLocation.isEmpty() && redirectCount < kMaxRedirects) {
        QUrl redirectUrl = currentUrl.resolved(QUrl::fromEncoded(redirectLocation));
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
        m_readPos(0),
        m_started(false)
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

    // Construction only builds the worker -- it does NOT start() it. Whether
    // this reply runs immediately or waits its turn is TlsNetworkAccessManager's
    // call (see its createRequest()/dispatchQueued()): letting every reply
    // start unconditionally here is exactly what let an unbounded burst of
    // sendToDevice/thumbnail requests during a busy sync pile up hundreds of
    // simultaneous TLS handshakes/threads and take the process down -- see
    // conversation.
    m_worker = new TlsRequestThread(method, request.url(), headers, outgoingData, this);
    connect(m_worker, SIGNAL(finished()), this, SLOT(onWorkerFinished()));

    int n = g_liveReplies.fetchAndAddRelaxed(1) + 1;
    bbportLog(QString("[BBport:tls] +reply %1 %2 live=%3").arg(method, request.url().toString()).arg(n));
}

TlsNetworkReply::~TlsNetworkReply()
{
    int n = g_liveReplies.fetchAndAddRelaxed(-1) - 1;
    bbportLog(QString("[BBport:tls] -reply %1 live=%2").arg(url().toString()).arg(n));
    if (m_worker) {
        m_worker->requestAbort();
        m_worker->wait();
    }
}

void TlsNetworkReply::startWorker()
{
    if (!m_worker || m_started) return;
    m_started = true;
    m_worker->start();
}

void TlsNetworkReply::abort()
{
    if (m_worker) {
        m_worker->requestAbort();
    }
    if (!m_started) {
        // Still sitting in TlsNetworkAccessManager's pending queue -- nothing
        // is running that will ever call onWorkerFinished()/emit finished()
        // on its own, so this has to do it here or the reply (and its queue
        // slot) would never be released.
        m_started = true;
        setError(QNetworkReply::OperationCanceledError, "Aborted before starting.");
        setFinished(true);
        emit finished();
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
    bbportLog(QString("[BBport:tls] worker finished %1 status=%2 err=%3")
                  .arg(url().toString()).arg(m_worker->httpStatus).arg(int(m_worker->netError)));

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
