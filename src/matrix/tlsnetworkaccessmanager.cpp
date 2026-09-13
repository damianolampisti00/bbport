#include "tlsnetworkaccessmanager.hpp"

#ifdef BBPORT_HAVE_NATIVE_TLS

#include "tlsnetworkreply.hpp"

#include <QUrl>
#include <QNetworkRequest>
#include <QDebug>

namespace {
// Not tuned beyond "clearly bigger than one, clearly smaller than the
// hundreds seen in the crash log" -- see conversation. Room to revisit if it
// turns out to be too conservative for e.g. a media-heavy room opening.
const int kMaxConcurrentTlsRequests = 6;
}

TlsNetworkAccessManager::TlsNetworkAccessManager(QObject *parent) :
        QNetworkAccessManager(parent),
        m_activeCount(0)
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
    TlsNetworkReply *reply = new TlsNetworkReply(op, request, body, this);
    connect(reply, SIGNAL(finished()), this, SLOT(onManagedReplyFinished()));

    // SyncEngine already keeps at most one /sync in flight at a time (see
    // its m_currentReply) -- queuing it behind a burst of secondary
    // requests (avatar thumbnails, room-key forwards) would stall the whole
    // sync loop for no benefit, so it always bypasses the cap.
    bool isSync = request.url().path().contains("/sync");

    if (isSync || m_activeCount < kMaxConcurrentTlsRequests) {
        ++m_activeCount;
        reply->startWorker();
    } else {
        m_pending.append(reply);
        // General runtime instrumentation (see conversation): the cap only
        // ever bites when this fires, so seeing how deep the queue gets
        // during real usage is what tells us whether kMaxConcurrentTlsRequests
        // is actually a bottleneck (e.g. media-heavy rooms loading slowly)
        // or comfortably sized.
        qDebug() << "[BBport:tls] queued (cap reached)" << request.url().toString()
                  << "pending=" << m_pending.size() << "active=" << m_activeCount;
    }
    return reply;
}

void TlsNetworkAccessManager::onManagedReplyFinished()
{
    TlsNetworkReply *reply = qobject_cast<TlsNetworkReply*>(sender());
    if (reply && m_pending.removeOne(reply)) {
        // Was aborted while still queued (never actually started, so never
        // held one of the active slots) -- nothing to free.
        return;
    }
    if (m_activeCount > 0) --m_activeCount;
    dispatchQueued();
}

void TlsNetworkAccessManager::dispatchQueued()
{
    while (!m_pending.isEmpty() && m_activeCount < kMaxConcurrentTlsRequests) {
        QPointer<TlsNetworkReply> next = m_pending.takeFirst();
        if (!next) continue; // destroyed while queued -- see m_pending's comment
        ++m_activeCount;
        next->startWorker();
    }
}

#endif /* BBPORT_HAVE_NATIVE_TLS */
