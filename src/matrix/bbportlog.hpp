#ifndef BBPORTLOG_HPP_
#define BBPORTLOG_HPP_

#include <QString>
#include <QByteArray>
#include <QMutex>
#include <QMutexLocker>
#include <QDateTime>
#include <QDebug>
#include <cstdio>

// General runtime diagnostics saved straight to a file (see conversation):
// installing a fresh build outside Momentics -- no live console attached at
// all -- means qDebug() output goes nowhere. Every qDebug() diagnostic line
// added this session (sync cycles, TLS thread/reply lifecycle, key-backup
// unlock errors, ...) now also goes through here, so the same information
// survives a standalone run and can be pulled off the device afterward
// (any on-device terminal, e.g. Term49: `cat
// /accounts/1000/shared/misc/bbport_debug.log`) -- same shared/misc
// convention MediaManager::debugLog() already uses, reusing the identical
// file so everything ends up in one chronological place.
//
// Header-only + inline so every .cpp that includes this shares the exact
// same QMutex instance (function-local statics in an inline function are
// merged across translation units) -- needed since TlsRequestThread calls
// this from background worker threads concurrently with the UI thread.
inline QMutex &bbportLogMutex()
{
    static QMutex m;
    return m;
}

inline void bbportLog(const QString &line)
{
    // Also goes through qDebug() -- harmless (and still useful) whenever
    // Momentics IS attached, but the file write is what actually matters
    // now that the normal workflow is installing a .bar standalone with no
    // console at all.
    qDebug() << qPrintable(line);
    QMutexLocker locker(&bbportLogMutex());
    FILE *f = fopen("/accounts/1000/shared/misc/bbport_debug.log", "a");
    if (!f) return;
    QByteArray utf8 = (QDateTime::currentDateTime().toString("hh:mm:ss.zzz") + " " + line + "\n").toUtf8();
    fwrite(utf8.constData(), 1, utf8.size(), f);
    fclose(f);
}

#endif /* BBPORTLOG_HPP_ */
