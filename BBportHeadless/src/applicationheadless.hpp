#ifndef APPLICATIONHEADLESS_HPP_
#define APPLICATIONHEADLESS_HPP_

#include <QObject>

namespace bb {
namespace system {
class InvokeManager;
class InvokeRequest;
}
}

class MatrixApi;
class KeyBackupManager;
class OlmCryptoManager;
class SyncEngine;
class NotificationManager;

// The real background sync service (see the persistent-login/headless-push
// plan): woken every ~15 minutes by the recurring timer ApplicationUI
// registers (bb::system::InvokeManager::registerTimer(), see
// applicationui.cpp), this does exactly one incremental /sync pass and
// posts a Hub notification for whatever arrived, then exits -- the
// short-running contract _sys_run_headless actually allows (see
// bar-descriptor.xml's own comment on why _sys_headless_nostop, tried
// first, never worked: that one needed a BlackBerry approval program that
// no longer exists now that BB10 is EOL).
//
// Mirrors ApplicationUI's own non-UI object graph (MatrixApi/
// KeyBackupManager/OlmCryptoManager/SyncEngine/NotificationManager) as
// closely as it can, but deliberately skips TimelineStore, MediaManager,
// RoomListModel and MessageListModel entirely -- none of them are needed
// just to decrypt an incoming message and post one notification, and
// RoomListModel specifically owns a bb::cascades::ArrayDataModel that isn't
// safe to construct outside a real Cascades Application (this process is a
// plain bb::Application, no Cascades bootstrap at all).
class ApplicationHeadless : public QObject
{
    Q_OBJECT
public:
    ApplicationHeadless();
    virtual ~ApplicationHeadless() {}

private slots:
    void onInvoked(const bb::system::InvokeRequest &request);
    void onLoginSucceeded();
    void onLoginFailed(const QString &error);
    void onSingleSyncFinished(bool ok);

private:
    bb::system::InvokeManager *m_invokeManager;
    MatrixApi *m_matrixApi;
    KeyBackupManager *m_keyBackupManager;
    OlmCryptoManager *m_olmCryptoManager;
    SyncEngine *m_syncEngine;
    NotificationManager *m_notificationManager;
};

#endif /* APPLICATIONHEADLESS_HPP_ */
