#include "applicationheadless.hpp"
#include "bbportlog.hpp"

#include <bb/system/InvokeManager>
#include <bb/system/InvokeRequest>
#include <QCoreApplication>

ApplicationHeadless::ApplicationHeadless() :
        QObject(),
        m_invokeManager(new bb::system::InvokeManager(this))
{
    // Logged unconditionally at construction (before any invoke is even
    // received) so the shared debug log distinguishes "the OS launched this
    // binary at all" from "it received the STARTED invoke" -- if only the
    // second line is missing on the next boot, that narrows the failure to
    // the invoke-target filter/action rather than the permissions or
    // packaging.
    bbportLog("[BBportHeadless] process started");

    connect(m_invokeManager, SIGNAL(invoked(bb::system::InvokeRequest)), this, SLOT(onInvoked(bb::system::InvokeRequest)));
}

void ApplicationHeadless::onInvoked(const bb::system::InvokeRequest &request)
{
    bbportLog(QString("[BBportHeadless] invoked action=%1 mimeType=%2")
                  .arg(request.action()).arg(request.mimeType()));

    // Short-running headless (_sys_run_headless, no _sys_headless_nostop --
    // see bar-descriptor.xml's invoke-target comment) means this process is
    // expected to do its bounded bit of work and exit, not stay resident
    // waiting for another invoke. Nothing to actually do yet at this smoke-
    // test stage beyond the log line above; quitting immediately proves this
    // half of the contract too, ahead of wiring in a real sync pass.
    QCoreApplication::instance()->quit();
}
