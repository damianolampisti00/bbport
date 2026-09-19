#include "applicationheadless.hpp"
#include "bbportlog.hpp"

#include "matrixapi.hpp"
#include "keybackupmanager.hpp"
#include "olmcryptomanager.hpp"
#include "syncengine.hpp"
#include "notificationmanager.hpp"

#include <bb/system/InvokeManager>
#include <bb/system/InvokeRequest>

#include <QCoreApplication>
#include <QTimer>
#include <QFile>
#include <QDir>

#include <sys/types.h>
#include <signal.h>
#include <errno.h>

namespace {

// True if ApplicationUI's own foreground.pid (see its own comment) names a
// still-running process. kill(pid, 0) sends no actual signal -- it's the
// standard POSIX way to just test whether a process exists: 0 means yes;
// -1/ESRCH means no; -1/EPERM means it exists but this process lacks
// permission to signal it, which still means "yes" for this purpose.
bool foregroundIsAlive()
{
    QFile pidFile(QDir::homePath() + "/foreground.pid");
    if (!pidFile.open(QIODevice::ReadOnly | QIODevice::Text)) return false;
    bool ok = false;
    int pid = pidFile.readAll().trimmed().toInt(&ok);
    if (!ok || pid <= 0) return false;
    if (::kill(pid_t(pid), 0) == 0) return true;
    return errno == EPERM;
}

} // namespace

ApplicationHeadless::ApplicationHeadless() :
        QObject(),
        m_invokeManager(new bb::system::InvokeManager(this)),
        m_matrixApi(0),
        m_keyBackupManager(0),
        m_olmCryptoManager(0),
        m_syncEngine(0),
        m_notificationManager(0)
{
    bbportLog("[BBportHeadless] process started");
    connect(m_invokeManager, SIGNAL(invoked(bb::system::InvokeRequest)), this, SLOT(onInvoked(bb::system::InvokeRequest)));

    // The foreground app, if open, is already continuously syncing and
    // already posts its own Hub notifications for whatever arrives in a
    // room that isn't the one currently open -- confirmed on a real device
    // that without this check, a headless wakeup landing while the
    // foreground app happened to already be open put both sides mid-/sync
    // at once, which could double-post the same notification from each
    // side independently, on top of the wasted redundant network activity.
    if (foregroundIsAlive()) {
        bbportLog("[BBportHeadless] foreground app already running, nothing to do");
        QTimer::singleShot(0, QCoreApplication::instance(), SLOT(quit()));
        return;
    }

    // Same non-UI object graph ApplicationUI itself builds (applicationui.cpp),
    // minus TimelineStore/MediaManager/RoomListModel/MessageListModel -- see
    // this class's own header comment for why.
    m_matrixApi = new MatrixApi(this);
    m_keyBackupManager = new KeyBackupManager(m_matrixApi, this);
    m_olmCryptoManager = new OlmCryptoManager(m_matrixApi, m_keyBackupManager, this);
    m_matrixApi->setPreferredDeviceId(m_olmCryptoManager->deviceId());
    m_syncEngine = new SyncEngine(m_matrixApi, m_keyBackupManager, m_olmCryptoManager, this);
    m_notificationManager = new NotificationManager(m_matrixApi, m_syncEngine, this);

    connect(m_matrixApi, SIGNAL(loginSucceeded()), m_olmCryptoManager, SLOT(start()));
    connect(m_matrixApi, SIGNAL(loginSucceeded()), this, SLOT(onLoginSucceeded()));
    connect(m_matrixApi, SIGNAL(loginFailed(QString)), this, SLOT(onLoginFailed(QString)));

    connect(m_syncEngine, SIGNAL(roomUpdated(QString,QVariantMap)), m_olmCryptoManager, SLOT(onRoomUpdated(QString,QVariantMap)));
    connect(m_syncEngine, SIGNAL(toDeviceEvent(QVariantMap)), m_olmCryptoManager, SLOT(handleToDeviceEvent(QVariantMap)));
    connect(m_syncEngine, SIGNAL(timelineEvent(QString,QVariantMap)), m_notificationManager, SLOT(onTimelineEvent(QString,QVariantMap)));
    connect(m_syncEngine, SIGNAL(singleSyncFinished(bool)), this, SLOT(onSingleSyncFinished(bool)));

    // tryAutoLogin() is a silent no-op if session.json doesn't exist (no
    // loginSucceeded/loginFailed either) -- nothing to sync in that case,
    // so there's nothing to wait for; quit right away rather than sitting
    // in the event loop forever. Deferred via singleShot(0, ...) rather
    // than calling quit() straight from the constructor, since a
    // QCoreApplication::exec() that hasn't started yet (main() hasn't
    // reached Application::exec() at this point) has nothing to quit.
    m_matrixApi->tryAutoLogin();
    if (!m_matrixApi->hasSavedSession()) {
        bbportLog("[BBportHeadless] no saved session, nothing to do");
        QTimer::singleShot(0, QCoreApplication::instance(), SLOT(quit()));
    }
}

void ApplicationHeadless::onInvoked(const bb::system::InvokeRequest &request)
{
    bbportLog(QString("[BBportHeadless] invoked action=%1 mimeType=%2")
                  .arg(request.action()).arg(request.mimeType()));
}

void ApplicationHeadless::onLoginSucceeded()
{
    bbportLog("[BBportHeadless] login ok, starting single sync pass");
    m_syncEngine->startOnce();
}

void ApplicationHeadless::onLoginFailed(const QString &error)
{
    // A revoked/expired token, or no network right now -- either way,
    // nothing more to do until the next scheduled wakeup 6 minutes from
    // now (or the user re-logs in via the foreground app, which writes a
    // fresh session.json this'll pick up next time).
    bbportLog("[BBportHeadless] login failed: " + error);
    QCoreApplication::instance()->quit();
}

void ApplicationHeadless::onSingleSyncFinished(bool ok)
{
    bbportLog(QString("[BBportHeadless] single sync finished ok=%1, quitting").arg(ok));
    QCoreApplication::instance()->quit();
}
