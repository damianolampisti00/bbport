#ifndef TLSNETWORKACCESSMANAGER_HPP_
#define TLSNETWORKACCESSMANAGER_HPP_

// Device-only -- see tlsnetworkreply.hpp for why this compiles to nothing
// when BBPORT_HAVE_NATIVE_TLS isn't defined (Simulator builds).
#ifdef BBPORT_HAVE_NATIVE_TLS

#include <QNetworkAccessManager>

// Routes https:// requests through TlsNetworkReply (mbedTLS-based, TLS 1.2)
// instead of Qt's QSslSocket backend, which can't be pointed at a modern
// OpenSSL on BB10 (see tlsnetworkreply.hpp for why). Any plain http://
// request would fall through to Qt's normal implementation unchanged, but
// nothing in this app makes one.
class TlsNetworkAccessManager : public QNetworkAccessManager
{
    Q_OBJECT
public:
    explicit TlsNetworkAccessManager(QObject *parent = 0);

protected:
    virtual QNetworkReply* createRequest(Operation op, const QNetworkRequest &request,
                                          QIODevice *outgoingData = 0);
};

#endif /* BBPORT_HAVE_NATIVE_TLS */

#endif /* TLSNETWORKACCESSMANAGER_HPP_ */
