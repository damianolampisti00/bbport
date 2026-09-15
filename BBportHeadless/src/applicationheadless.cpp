#include "applicationheadless.hpp"
#include "bbportlog.hpp"

#include <bb/system/InvokeManager>
#include <bb/system/InvokeRequest>

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
}
