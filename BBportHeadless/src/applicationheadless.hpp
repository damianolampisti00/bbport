#ifndef APPLICATIONHEADLESS_HPP_
#define APPLICATIONHEADLESS_HPP_

#include <QObject>

namespace bb {
namespace system {
class InvokeManager;
class InvokeRequest;
}
}

// Step 4 of the persistent-login/headless-push plan: the smallest possible
// slice, deliberately doing nothing but prove the mechanism itself works on
// a real device before any real sync code is written here -- BB10-specific
// assumptions (per-app sandboxing, missing ffmpeg encoders, QML property
// semantics) have only ever surfaced on real hardware this session, and the
// _sys_run_headless/_sys_headless_nostop permissions plus
// bb.action.system.STARTED firing on this specific device/OS build have
// never been tried in this project. Once this is confirmed working, this
// class grows into the real background sync service (MatrixApi/SyncEngine/
// NotificationManager, mirroring ApplicationUI's own object graph).
class ApplicationHeadless : public QObject
{
    Q_OBJECT
public:
    ApplicationHeadless();
    virtual ~ApplicationHeadless() {}

private slots:
    void onInvoked(const bb::system::InvokeRequest &request);

private:
    bb::system::InvokeManager *m_invokeManager;
};

#endif /* APPLICATIONHEADLESS_HPP_ */
