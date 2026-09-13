#ifndef TLSNETWORKACCESSMANAGER_HPP_
#define TLSNETWORKACCESSMANAGER_HPP_

// Device-only -- see tlsnetworkreply.hpp for why this compiles to nothing
// when BBPORT_HAVE_NATIVE_TLS isn't defined (Simulator builds).
#ifdef BBPORT_HAVE_NATIVE_TLS

#include <QNetworkAccessManager>
#include <QList>
#include <QPointer>

class TlsNetworkReply;

// Routes https:// requests through TlsNetworkReply (mbedTLS-based, TLS 1.2)
// instead of Qt's QSslSocket backend, which can't be pointed at a modern
// OpenSSL on BB10 (see tlsnetworkreply.hpp for why). Any plain http://
// request would fall through to Qt's normal implementation unchanged, but
// nothing in this app makes one.
//
// Caps how many TlsNetworkReply workers actually run at once (see
// createRequest()) -- see conversation for why: a sync response with a big
// backlog of undecryptable messages plus many room avatars to fetch used to
// fire every m.room_key_request/thumbnail request the instant it was known
// about, with zero limit, reaching 300+ simultaneous TLS handshakes/threads
// and taking the process down on resource exhaustion. Extra requests beyond
// the cap wait in m_pending and start as active ones finish.
class TlsNetworkAccessManager : public QNetworkAccessManager
{
    Q_OBJECT
public:
    explicit TlsNetworkAccessManager(QObject *parent = 0);

protected:
    virtual QNetworkReply* createRequest(Operation op, const QNetworkRequest &request,
                                          QIODevice *outgoingData = 0);

private slots:
    void onManagedReplyFinished();

private:
    void dispatchQueued();

    int m_activeCount;
    // QPointer, not a plain pointer: a queued reply can be destroyed (its
    // owner gave up on it) before ever being dispatched -- e.g. a room's
    // avatar ImageResponse getting torn down while still queued -- and this
    // must not dispatchQueued() into a dangling TlsNetworkReply*.
    QList<QPointer<TlsNetworkReply> > m_pending;
};

#endif /* BBPORT_HAVE_NATIVE_TLS */

#endif /* TLSNETWORKACCESSMANAGER_HPP_ */
