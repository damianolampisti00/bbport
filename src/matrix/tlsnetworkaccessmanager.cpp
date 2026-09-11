#include "tlsnetworkaccessmanager.hpp"

#ifdef BBPORT_HAVE_NATIVE_TLS

#include "tlsnetworkreply.hpp"

#include <QUrl>
#include <QNetworkRequest>

TlsNetworkAccessManager::TlsNetworkAccessManager(QObject *parent) :
        QNetworkAccessManager(parent)
{
}

QNetworkReply* TlsNetworkAccessManager::createRequest(Operation op, const QNetworkRequest &request,
                                                       QIODevice *outgoingData)
{
    if (request.url().scheme().compare("https", Qt::CaseInsensitive) != 0) {
        return QNetworkAccessManager::createRequest(op, request, outgoingData);
    }

    QByteArray body;
    if (outgoingData) {
        body = outgoingData->readAll();
    }
    return new TlsNetworkReply(op, request, body, this);
}

#endif /* BBPORT_HAVE_NATIVE_TLS */
