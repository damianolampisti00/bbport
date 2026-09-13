#ifndef TLSNETWORKREPLY_HPP_
#define TLSNETWORKREPLY_HPP_

// Device-only: mbedTLS is vendored for armv7 only (see third_party/mbedtls
// and the device{} block in BBport.pro). Simulator builds compile this
// translation unit down to nothing so config.pri's IDE-managed, config-wide
// source list can list it unconditionally without breaking Simulator-Debug.
#ifdef BBPORT_HAVE_NATIVE_TLS

#include <QNetworkReply>
#include <QNetworkRequest>
#include <QNetworkAccessManager>
#include <QThread>
#include <QMutex>
#include <QUrl>
#include <QByteArray>
#include <QList>
#include <QPair>

#include "mbedtls/net_sockets.h"

// Performs one blocking HTTPS request (TCP connect + TLS 1.2 handshake via
// mbedTLS + a hand-rolled HTTP/1.1 request/response) on a background thread.
// BB10's system OpenSSL only speaks TLS 1.0, which matrix.beeper.com rejects
// outright, and Qt 4.8's QSslSocket can't be pointed at a newer OpenSSL (its
// glue code assumes 0.9.8/1.0.x struct layouts) -- see TlsNetworkAccessManager
// for how this replaces QNetworkAccessManager's built-in HTTPS backend.
//
// A dedicated thread per request (rather than a hand-rolled non-blocking
// state machine driven off QSocketNotifier) keeps the mbedTLS/HTTP handling
// straightforward to get right; request volume here is low (a handful of API
// calls plus one long-lived /sync poll), so the extra thread per request is
// not a real cost.
class TlsRequestThread : public QThread
{
    Q_OBJECT
public:
    TlsRequestThread(const QString &method, const QUrl &url,
                      const QList<QPair<QByteArray, QByteArray> > &headers,
                      const QByteArray &body, QObject *parent = 0);
    virtual ~TlsRequestThread();

    // Callable from another thread (the reply's abort()) to unblock a
    // pending mbedtls_net_recv/send by closing the underlying socket.
    void requestAbort();

    // Public (not just for run()'s own use) so the free helper functions in
    // tlsnetworkreply.cpp's anonymous namespace (sslReadMore/ensureBytes) can
    // poll it themselves between retries -- see the comment on sslReadMore
    // for why relying on requestAbort()'s shutdown() alone wasn't enough.
    bool wasAborted();

    int httpStatus;
    QByteArray responseBody;
    QNetworkReply::NetworkError netError;
    QString netErrorString;

protected:
    virtual void run();

private:
    QString m_method;
    QUrl m_url;
    QList<QPair<QByteArray, QByteArray> > m_headers;
    QByteArray m_body;

    QMutex m_netMutex;
    mbedtls_net_context m_netCtx;
    bool m_aborted;
};

class TlsNetworkReply : public QNetworkReply
{
    Q_OBJECT
public:
    TlsNetworkReply(QNetworkAccessManager::Operation op, const QNetworkRequest &request,
                     const QByteArray &outgoingData, QObject *parent = 0);
    virtual ~TlsNetworkReply();

    // Actually starts the worker thread. Construction alone never does --
    // TlsNetworkAccessManager decides when (immediately, or once a
    // concurrency-limit slot frees up); see its createRequest().
    void startWorker();

    virtual void abort();
    virtual qint64 bytesAvailable() const;

protected:
    virtual qint64 readData(char *data, qint64 maxlen);

private slots:
    void onWorkerFinished();

private:
    TlsRequestThread *m_worker;
    QByteArray m_buffer;
    qint64 m_readPos;
    bool m_started;
};

#endif /* BBPORT_HAVE_NATIVE_TLS */

#endif /* TLSNETWORKREPLY_HPP_ */
