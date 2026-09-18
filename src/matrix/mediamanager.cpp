#include "mediamanager.hpp"
#include "matrixapi.hpp"
#include "oggopusdecoder.hpp"
#include "oggopusencoder.hpp"
#include "bbportlog.hpp"

#include <bb/data/JsonDataAccess>

#include <olm/olm.h>
#include <olm/crypto.h>
#include <olm/base64.h>

extern "C" {
#include <crypto-algorithms/aes.h>
}

#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QProcessEnvironment>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QCryptographicHash>
#include <QRegExp>
#include <QUrl>
#include <QDateTime>
#include <QTextStream>
#include <QtCore/qendian.h>
#include <cstdlib>

#include <bb/system/InvokeManager>
#include <bb/system/InvokeRequest>

using namespace bb::data;

// Video transcoding and Instagram media extraction used to be delegated to
// a dev-machine PC proxy script (long since removed); both features moved
// on-device using BerryCore's (github.com/sw7ft/BerryCore) bundled ffmpeg,
// plus parth-dl (github.com/parthmax2/parth-dl, `pip install parth-dl` on
// this same Python) for Instagram itself, invoked directly via QProcess.
// No PC involvement needed any more.
//
// parth-dl replaced an initial yt-dlp-based implementation entirely.
// yt-dlp's own Instagram extractor can't download a mixed carousel's
// photo entries at all (tries to resolve "video formats" for every
// sidecar child and errors out on the photo ones -- a known, maintainer-
// closed-wontfix limitation, github.com/yt-dlp/yt-dlp issue #7569, traced
// down to the extractor source: a photo child is never given downloadable
// formats no matter which URL reaches it). Several hand-rolled workarounds
// (Instagram's own "?img_index=N" link, each child's own shortcode as a
// permalink, scraping the plain post page's HTML for an embedded
// "display_url" JSON blob, calling Instagram's legacy GraphQL endpoint
// directly with the public X-IG-App-ID header plus a primed anonymous
// session's csrftoken/cookies) all dead-ended on a real device -- the
// GraphQL attempt got a flat HTTP 401 even with a fully primed guest
// session, confirming that endpoint now needs more than anonymous access.
// parth-dl's own `get_info(url)` Python API, by contrast, was confirmed by
// hand (on a real machine) to return every sidecar child's real CDN URL --
// video and photos alike, for the exact same mixed carousel -- with zero
// login, so BBport now shells out to that library for every Instagram
// fetch instead of re-deriving Instagram's extraction internals by hand.
static const char *kBerryCoreRoot = "/accounts/1000/shared/misc/berrycore";
static const char *kBerryCoreFfmpeg = "/accounts/1000/shared/misc/berrycore/bin/ffmpeg";
static const char *kBerryCorePython3 = "/accounts/1000/shared/misc/berrycore/bin/python3";

// Fetches every media entry of an Instagram post (a Reel/video post has
// exactly one; a carousel has one per slide) via parth-dl's get_info()
// API, then downloads each one itself rather than trusting parth-dl's own
// file-writing -- confirmed on a real machine that parth-dl names photo
// files ".heic" purely off the URL path even when the CDN actually serves
// a plain JPEG (real "Content-Type: image/jpeg" header and a genuine JPEG
// SOI marker, FF D8 FF E0, despite the misleading URL -- Instagram
// transcodes on the fly per the URL's own "stp=dst-jpg_..." query
// parameter). BB10 has no HEIC decoder at all, so this saves each file
// under whichever extension its *actual* returned bytes/Content-Type say,
// never the URL's own claimed one, falling back to real "heic" only if
// the bytes genuinely are (a private/restricted post parth-dl couldn't
// get a JPEG transcode for, say).
//
// Invoked as `python3 -c <this> <postUrl> <outPrefix>`; writes each entry
// to "<outPrefix>_<NN>.<ext>" (1-based, matching carousel slide order),
// plus a plain-text "<outPrefix>_<NN>.dims" sidecar ("<width> <height>",
// parth-dl already reports both for every format) that
// finishInstagramFetch() folds into that item's width/height and then
// deletes -- QML uses real dimensions to size the video surface without
// stretching (see carouselViewerPage's fwcCarouselVideoSurface), instead
// of the fixed square used when an item's real size isn't known. Prints
// one "OK N kind ext WxH url" / "FAIL N reason" / "MISSING_INDEX N" line
// per entry for debugLog to capture.
static const char *kInstagramFetchScript =
    "import sys, ssl, urllib.request\n"
    "\n"
    "url, out_prefix = sys.argv[1], sys.argv[2]\n"
    "\n"
    "ctx = ssl.create_default_context()\n"
    "ctx.check_hostname = False\n"
    "ctx.verify_mode = ssl.CERT_NONE\n"
    "ua = 'Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/124.0 Safari/537.36'\n"
    "\n"
    "# `pip install --user` inside a shell app (Term49) lands in THAT app's\n"
    "# own private appdata sandbox -- confirmed on a real device to be\n"
    "# invisible to a completely different app's (BBport's) child process,\n"
    "# a BB10 OS-level restriction no PYTHONPATH trick can cross. This one\n"
    "# extra shared directory is BOTH readable from any app (same tree\n"
    "# BerryCore's own ffmpeg/python3 already live in, already proven\n"
    "# readable from here) AND where the install instructions point pip's\n"
    "# own --target at, so the two sides are guaranteed to agree.\n"
    "sys.path.insert(0, '/accounts/1000/shared/misc/berrycore/pylib')\n"
    "\n"
    "try:\n"
    "    from parth_dl import get_info\n"
    "    info = get_info(url)\n"
    "except Exception as e:\n"
    "    print('PARTH_DL_FAILED %s' % e)\n"
    "    sys.exit(1)\n"
    "\n"
    "entries = info.get('entries') or []\n"
    "print('parth-dl get_info ok entries=%d' % len(entries))\n"
    "\n"
    "for i, entry in enumerate(entries):\n"
    "    idx = i + 1\n"
    "    kind = entry.get('kind') or 'image'\n"
    "    formats = entry.get('formats') or []\n"
    "    item_url = formats[0].get('url') if formats else None\n"
    "    if not item_url:\n"
    "        print('MISSING_INDEX %d' % idx)\n"
    "        continue\n"
    "    try:\n"
    "        req = urllib.request.Request(item_url, headers={'User-Agent': ua})\n"
    "        resp = urllib.request.urlopen(req, context=ctx, timeout=30)\n"
    "        data = resp.read()\n"
    "        ctype = resp.headers.get('Content-Type', '')\n"
    "        if kind == 'video':\n"
    "            ext = 'mp4'\n"
    "        elif 'png' in ctype or data[:8] == b'\\x89PNG\\r\\n\\x1a\\n':\n"
    "            ext = 'png'\n"
    "        elif 'heic' in ctype or 'heif' in ctype:\n"
    "            ext = 'heic'\n"
    "        else:\n"
    "            ext = 'jpg'\n"
    "        with open('%s_%02d.%s' % (out_prefix, idx, ext), 'wb') as f:\n"
    "            f.write(data)\n"
    "        w, h = formats[0].get('width') or 0, formats[0].get('height') or 0\n"
    "        with open('%s_%02d.dims' % (out_prefix, idx), 'w') as f:\n"
    "            f.write('%d %d' % (w, h))\n"
    "        print('OK %d %s %s %dx%d %s' % (idx, kind, ext, w, h, item_url[:120]))\n"
    "    except Exception as e:\n"
    "        print('FAIL %d %s' % (idx, e))\n";

// ffmpeg needs BerryCore's LD_LIBRARY_PATH (built against its bundled QNX
// target tree, not BBNDK's). Mirrors berrycore/env.sh exactly.
//
// SSL_CERT_FILE is the standard OpenSSL/Python env var most TLS libraries
// (including stdlib ssl.create_default_context(), which parth-dl's own
// networking uses) check for a CA bundle -- confirmed necessary on a real
// device: parth-dl's own HTTPS calls failed with "CERTIFICATE_VERIFY_
// FAILED: unable to get local issuer certificate" without it, this
// environment's Python having no usable system trust store of its own
// (the same underlying gap --no-check-certificate papered over for
// yt-dlp). Pointing it at BBport's own already-bundled, already-proven-
// working CA bundle (the exact same file tlsnetworkreply.cpp's mbedTLS
// layer parses) fixes every Python HTTPS call spawned through this
// environment at once, with no per-library workaround needed.
static QProcessEnvironment berryCoreEnvironment()
{
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    QString root = QString::fromLatin1(kBerryCoreRoot);
    QString path = root + "/bin:" + root + "/sbin:" + env.value("PATH");
    QString ldLibraryPath = root + "/target_10_3_1_995/qnx6/armle-v7/usr/lib:" + root + "/lib:" + env.value("LD_LIBRARY_PATH");
    env.insert("PATH", path);
    env.insert("LD_LIBRARY_PATH", ldLibraryPath);
    env.insert("SSL_CERT_FILE", QDir::currentPath() + "/app/native/assets/cacert.pem");
    return env;
}

// Matrix uses unpadded standard base64 almost everywhere, but the
// EncryptedFile JWK key ("file".key.k) is unpadded base64URL (RFC 4648 §5:
// '-'/'_' instead of '+'/'/') -- olm's own decoder expects the standard
// alphabet, so translate before decoding. Same helper pattern as
// KeyBackupManager's SSSS unwrap (kept separate since these are different
// translation units and this one needs the url-safe variant too).
static QByteArray olmBase64Decode(QByteArray input, bool urlSafe = false)
{
    if (urlSafe) {
        input.replace('-', '+');
        input.replace('_', '/');
    }
    size_t rawLen = _olm_decode_base64_length(input.size());
    if (rawLen == (size_t)-1) return QByteArray();
    QByteArray out(int(rawLen), '\0');
    _olm_decode_base64((const uint8_t*)input.constData(), input.size(), (uint8_t*)out.data());
    return out;
}

MediaManager::MediaManager(MatrixApi *api, QObject *parent) :
        QObject(parent),
        m_api(api),
        m_invokeManager(new bb::system::InvokeManager(this)),
        m_recordingCounter(0)
{
    m_cacheDir = QDir::homePath() + "/matrix_media";
    QDir().mkpath(m_cacheDir);
}

MediaManager::~MediaManager()
{
}

bool MediaManager::splitMxc(const QString &mxcUri, QString *server, QString *mediaId)
{
    if (!mxcUri.startsWith("mxc://")) return false;
    QString rest = mxcUri.mid(QString("mxc://").length());
    int slashIdx = rest.indexOf('/');
    if (slashIdx < 0) return false;
    *server = rest.left(slashIdx);
    *mediaId = rest.mid(slashIdx + 1);
    return !server->isEmpty() && !mediaId->isEmpty();
}

QString MediaManager::cachePathFor(const QString &mxcUri) const
{
    QByteArray hash = QCryptographicHash::hash(mxcUri.toUtf8(), QCryptographicHash::Md5).toHex();
    return m_cacheDir + "/" + QString::fromLatin1(hash);
}

QString MediaManager::cachePathForThumbnail(const QString &mxcUri, int width, int height) const
{
    QString key = QString("%1|thumb|%2x%3").arg(mxcUri).arg(width).arg(height);
    QByteArray hash = QCryptographicHash::hash(key.toUtf8(), QCryptographicHash::Md5).toHex();
    return m_cacheDir + "/" + QString::fromLatin1(hash);
}

QString MediaManager::resolve(const QString &mxcUri, const QString &key, const QString &iv, const QString &sha256, bool isVideo)
{
    if (mxcUri.isEmpty()) return QString();

    QString cachePath = cachePathFor(mxcUri);
    if (QFile::exists(cachePath)) {
        return "file://" + cachePath;
    }

    if (m_inFlight.contains(mxcUri)) return QString();

    QString server, mediaId;
    if (!splitMxc(mxcUri, &server, &mediaId)) return QString();

    if (!key.isEmpty()) {
        CryptoInfo info;
        info.key = key;
        info.iv = iv;
        info.sha256 = sha256;
        m_cryptoInfo[mxcUri] = info;
    }
    if (isVideo) m_videoDownload.insert(mxcUri);

    QString downloadPath = QString("/_matrix/media/r0/download/%1/%2").arg(server, mediaId);
    QNetworkReply *reply = m_api->rawGet(downloadPath);
    m_inFlight.insert(mxcUri);
    m_downloadTarget[reply] = mxcUri;
    connect(reply, SIGNAL(finished()), this, SLOT(onDownloadFinished()));
    return QString();
}

bool MediaManager::decryptFile(const QByteArray &ciphertext, const CryptoInfo &info, QByteArray *plaintextOut)
{
    QByteArray aesKey = olmBase64Decode(info.key.toUtf8(), true /* url-safe */);
    QByteArray ivBytes = olmBase64Decode(info.iv.toUtf8(), false);
    if (aesKey.size() != 32 || ivBytes.size() != 16) return false;

    if (!info.sha256.isEmpty()) {
        void *utilMem = std::malloc(olm_utility_size());
        OlmUtility *util = olm_utility(utilMem);
        QByteArray computed(int(olm_sha256_length(util)), '\0');
        olm_sha256(util, ciphertext.constData(), ciphertext.size(), computed.data(), computed.size());
        olm_clear_utility(util);
        std::free(utilMem);
        if (QString::fromUtf8(computed) != info.sha256) return false;
    }

    WORD keySchedule[60];
    aes_key_setup((const BYTE*)aesKey.constData(), keySchedule, 256);
    plaintextOut->resize(ciphertext.size());
    aes_decrypt_ctr((const BYTE*)ciphertext.constData(), ciphertext.size(),
            (BYTE*)plaintextOut->data(), keySchedule, 256, (const BYTE*)ivBytes.constData());
    return true;
}

void MediaManager::onDownloadFinished()
{
    QNetworkReply *reply = qobject_cast<QNetworkReply*>(sender());
    if (!reply) return;
    QString mxcUri = m_downloadTarget.take(reply);
    CryptoInfo crypto = m_cryptoInfo.take(mxcUri);
    bool isVideo = m_videoDownload.remove(mxcUri);
    // m_inFlight is deliberately NOT cleared here when isVideo -- it stays
    // held until the transcode round-trip below also finishes, so a
    // re-render mid-transcode (resolve() called again for the same mxcUri)
    // doesn't kick off a duplicate download.

    if (reply->error() != QNetworkReply::NoError) {
        m_inFlight.remove(mxcUri);
        reply->deleteLater();
        emit mediaFailed(mxcUri);
        return;
    }

    QByteArray data = reply->readAll();
    reply->deleteLater();

    if (!crypto.key.isEmpty()) {
        QByteArray plaintext;
        if (!decryptFile(data, crypto, &plaintext)) {
            m_inFlight.remove(mxcUri);
            emit mediaFailed(mxcUri);
            return;
        }
        data = plaintext;
    }

    // Voice notes from bridged contacts (WhatsApp/Signal via Beeper) arrive
    // as Ogg-Opus, which BB10's own MediaPlayer cannot play at all (no Opus
    // codec support on this OS) -- caught here by content sniffing (the
    // "OggS" magic), not by mimetype, since mimetype isn't threaded through
    // to this method. Transcoded to a plain PCM WAV file that MediaPlayer
    // can play; if decoding fails, the message is treated as a failed
    // download rather than caching an unplayable file.
    if (data.startsWith("OggS")) {
        QByteArray wav;
        if (!OggOpusDecoder::decodeToWav(data, &wav)) {
            m_inFlight.remove(mxcUri);
            emit mediaFailed(mxcUri);
            return;
        }
        data = wav;
    }

    if (isVideo) {
        // The Q5's hardware video decoder can't handle the resolution/
        // profile most phones export by default (audio plays fine, video
        // stays black -- a known BB10 platform limitation, not something
        // fixable on-device). Re-encoded right here via BerryCore's ffmpeg
        // into a profile the Q5 can actually decode.
        QByteArray hash = QCryptographicHash::hash(mxcUri.toUtf8(), QCryptographicHash::Md5).toHex();
        QString base = m_cacheDir + "/tmp_" + QString::fromLatin1(hash);
        QString inPath = base + "_in";
        QString outPath = base + "_out";
        QFile::remove(outPath);

        QFile inFile(inPath);
        if (!inFile.open(QIODevice::WriteOnly)) {
            m_inFlight.remove(mxcUri);
            emit mediaFailed(mxcUri);
            return;
        }
        inFile.write(data);
        inFile.close();

        FfmpegJob job;
        job.mxcUri = mxcUri;
        job.inPath = inPath;
        job.outPath = outPath;

        QStringList args;
        args << "-y" << "-i" << inPath
             << "-vf" << "scale='min(1280,iw)':-2"
             << "-c:v" << "libx264" << "-profile:v" << "high" << "-level" << "4.0"
             << "-b:v" << "2500k" << "-maxrate" << "2500k" << "-bufsize" << "5000k"
             << "-c:a" << "aac" << "-b:a" << "128k" << "-ar" << "44100"
             << "-movflags" << "+faststart"
             << outPath;

        QProcess *proc = new QProcess(this);
        proc->setProcessEnvironment(berryCoreEnvironment());
        m_ffmpegJobs[proc] = job;
        connect(proc, SIGNAL(finished(int,QProcess::ExitStatus)), this, SLOT(onFfmpegFinished(int,QProcess::ExitStatus)));
        connect(proc, SIGNAL(error(QProcess::ProcessError)), this, SLOT(onFfmpegError(QProcess::ProcessError)));
        proc->start(QString::fromLatin1(kBerryCoreFfmpeg), args);
        return;
    }

    m_inFlight.remove(mxcUri);
    QString cachePath = cachePathFor(mxcUri);
    QFile file(cachePath);
    if (!file.open(QIODevice::WriteOnly)) {
        emit mediaFailed(mxcUri);
        return;
    }
    file.write(data);
    file.close();

    emit mediaReady(mxcUri, "file://" + cachePath);
}

void MediaManager::onFfmpegFinished(int exitCode, QProcess::ExitStatus exitStatus)
{
    QProcess *proc = qobject_cast<QProcess*>(sender());
    if (!proc || !m_ffmpegJobs.contains(proc)) return; // error() already handled it
    finishFfmpegJob(proc, exitStatus == QProcess::NormalExit && exitCode == 0);
}

void MediaManager::onFfmpegError(QProcess::ProcessError error)
{
    Q_UNUSED(error);
    QProcess *proc = qobject_cast<QProcess*>(sender());
    // A QProcess that fails to start never emits finished(), so this is the
    // only signal we get in that case -- treat it the same as a nonzero
    // exit. On a crash Qt emits both error() and finished(); this guard
    // ensures only whichever fires first actually consumes the job.
    if (!proc || !m_ffmpegJobs.contains(proc)) return;
    finishFfmpegJob(proc, false);
}

// Falls back to caching the original, untranscoded video on any ffmpeg
// failure (missing binary, unsupported input, crash, ...) rather than
// failing the download outright -- NativeVideoPlayer's mm-renderer wrapper
// plays arbitrary H.264/etc. content directly, so an untranscoded video
// still works, just without the predictable size/profile ffmpeg gives it.
void MediaManager::finishFfmpegJob(QProcess *proc, bool succeeded)
{
    FfmpegJob job = m_ffmpegJobs.take(proc);
    proc->deleteLater();
    m_inFlight.remove(job.mxcUri);

    QString cachePath = cachePathFor(job.mxcUri);
    QString sourcePath = (succeeded && QFile::exists(job.outPath)) ? job.outPath : job.inPath;

    bool ok = QFile::exists(sourcePath) && QFile::copy(sourcePath, cachePath);
    QFile::remove(job.inPath);
    QFile::remove(job.outPath);

    if (!ok) {
        emit mediaFailed(job.mxcUri);
        return;
    }
    emit mediaReady(job.mxcUri, "file://" + cachePath);
}

QString MediaManager::fetchInstagramVideo(const QString &instagramUrl)
{
    if (instagramUrl.isEmpty()) return QString();

    QString cachePath = cachePathFor(instagramUrl);
    if (QFile::exists(cachePath)) {
        return "file://" + cachePath;
    }
    if (m_inFlight.contains(instagramUrl)) return QString();
    m_inFlight.insert(instagramUrl);

    startInstagramFetch(instagramUrl, instagramUrl, cachePath + "_dl", false);
    return QString();
}

QString MediaManager::carouselBaseUrl(const QString &instagramUrl)
{
    int qIdx = instagramUrl.indexOf('?');
    return qIdx >= 0 ? instagramUrl.left(qIdx) : instagramUrl;
}

QString MediaManager::carouselCacheKey(const QString &baseUrl) const
{
    QByteArray hash = QCryptographicHash::hash(("carousel|" + baseUrl).toUtf8(), QCryptographicHash::Md5).toHex();
    return m_cacheDir + "/" + QString::fromLatin1(hash);
}

QString MediaManager::carouselManifestPath(const QString &baseUrl) const
{
    return carouselCacheKey(baseUrl) + "_manifest.json";
}

QVariantList MediaManager::loadCarouselManifest(const QString &baseUrl) const
{
    QVariantList empty;
    QFile f(carouselManifestPath(baseUrl));
    if (!f.open(QIODevice::ReadOnly)) return empty;
    QByteArray data = f.readAll();
    f.close();

    JsonDataAccess jda;
    QVariant parsed = jda.loadFromBuffer(data);
    if (jda.hasError()) return empty;

    QVariantList items = parsed.toList();
    for (int i = 0; i < items.size(); ++i) {
        QString path = items.at(i).toMap().value("url").toString();
        path.remove("file://");
        if (!QFile::exists(path)) return empty; // stale/partial cache -- refetch rather than serve broken paths
    }
    return items;
}

void MediaManager::saveCarouselManifest(const QString &baseUrl, const QVariantList &items) const
{
    JsonDataAccess jda;
    QByteArray buffer;
    jda.saveToBuffer(QVariant(items), &buffer);
    QFile f(carouselManifestPath(baseUrl));
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        f.write(buffer);
    }
}

QVariantList MediaManager::fetchInstagramCarousel(const QString &instagramUrl)
{
    if (instagramUrl.isEmpty()) return QVariantList();

    QString baseUrl = carouselBaseUrl(instagramUrl);
    QVariantList cached = loadCarouselManifest(baseUrl);
    if (!cached.isEmpty()) return cached;

    QString inFlightKey = "carousel|" + baseUrl;
    if (m_inFlight.contains(inFlightKey)) return QVariantList();
    m_inFlight.insert(inFlightKey);

    startInstagramFetch(instagramUrl, baseUrl, carouselCacheKey(baseUrl) + "_dl", true);
    return QVariantList();
}

// Shared by fetchInstagramVideo() and fetchInstagramCarousel(): both are
// just "fetch every media entry of this Instagram post via parth-dl" with
// a different cache/signal shape on top (see finishInstagramFetch()).
// fetchUrl is the URL actually handed to parth-dl (query string stripped
// for a carousel -- see carouselBaseUrl()'s doc comment -- unchanged for a
// Reel link); instagramUrl is the exact link callers/signals identify the
// request by.
void MediaManager::startInstagramFetch(const QString &instagramUrl, const QString &fetchUrl, const QString &outPrefix, bool isCarousel)
{
    // Clear out any leftover from a previous run that never made it to
    // cleanup (e.g. the app was killed mid-fetch) -- otherwise the glob in
    // finishInstagramFetch() could match a stale file instead of (or
    // alongside) the one this run is about to produce.
    {
        QFileInfo prefixInfo(outPrefix);
        QStringList leftovers = QDir(prefixInfo.absolutePath())
                .entryList(QStringList() << (prefixInfo.fileName() + "_*"), QDir::Files);
        foreach (const QString &name, leftovers) {
            QFile::remove(prefixInfo.absolutePath() + "/" + name);
        }
    }

    QStringList args;
    args << "-c" << QString::fromLatin1(kInstagramFetchScript) << fetchUrl << outPrefix;

    debugLog(QString("parth-dl starting (%1): %2").arg(isCarousel ? "carousel" : "video").arg(fetchUrl));

    QProcess *proc = new QProcess(this);
    proc->setProcessEnvironment(berryCoreEnvironment());
    InstagramFetchJob job;
    job.instagramUrl = instagramUrl;
    job.outPrefix = outPrefix;
    job.isCarousel = isCarousel;
    m_instagramFetchJobs[proc] = job;
    connect(proc, SIGNAL(finished(int,QProcess::ExitStatus)), this, SLOT(onInstagramFetchFinished(int,QProcess::ExitStatus)));
    connect(proc, SIGNAL(error(QProcess::ProcessError)), this, SLOT(onInstagramFetchError(QProcess::ProcessError)));
    proc->start(QString::fromLatin1(kBerryCorePython3), args);

    // Guarantees finishInstagramFetch() always eventually runs even if the
    // Python process itself never exits (e.g. Instagram serves a login/
    // consent wall parth-dl's own retry logic doesn't recognize).
    QTimer *watchdog = new QTimer(this);
    watchdog->setSingleShot(true);
    m_instagramProcTimer[proc] = watchdog;
    m_instagramTimeoutTarget[watchdog] = proc;
    connect(watchdog, SIGNAL(timeout()), this, SLOT(onInstagramFetchTimeout()));
    watchdog->start(45000);
}

void MediaManager::onInstagramFetchTimeout()
{
    QTimer *watchdog = qobject_cast<QTimer*>(sender());
    if (!watchdog) return;
    QProcess *proc = m_instagramTimeoutTarget.take(watchdog);
    watchdog->deleteLater();
    if (!proc || !m_instagramFetchJobs.contains(proc)) return; // already finished normally
    m_instagramProcTimer.remove(proc);
    debugLog("parth-dl timed out after 45s -- killing and failing the fetch");
    proc->kill();
    finishInstagramFetch(proc, false);
}

void MediaManager::onInstagramFetchFinished(int exitCode, QProcess::ExitStatus exitStatus)
{
    QProcess *proc = qobject_cast<QProcess*>(sender());
    if (!proc || !m_instagramFetchJobs.contains(proc)) return; // error() already handled it
    finishInstagramFetch(proc, exitStatus == QProcess::NormalExit && exitCode == 0);
}

void MediaManager::onInstagramFetchError(QProcess::ProcessError error)
{
    QProcess *proc = qobject_cast<QProcess*>(sender());
    if (!proc || !m_instagramFetchJobs.contains(proc)) return;
    debugLog(QString("parth-dl process error: %1 (%2)").arg(int(error)).arg(proc->errorString()));
    finishInstagramFetch(proc, false);
}

void MediaManager::finishInstagramFetch(QProcess *proc, bool succeeded)
{
    InstagramFetchJob job = m_instagramFetchJobs.take(proc);

    QTimer *watchdog = m_instagramProcTimer.take(proc);
    if (watchdog) {
        m_instagramTimeoutTarget.remove(watchdog);
        watchdog->stop();
        watchdog->deleteLater();
    }

    debugLog(QString("parth-dl finished url=%1 succeeded=%2 exitCode=%3")
                 .arg(job.instagramUrl).arg(succeeded).arg(proc->exitCode()));
    QString stdOut = QString::fromUtf8(proc->readAllStandardOutput());
    QString stdErr = QString::fromUtf8(proc->readAllStandardError());
    if (!stdOut.isEmpty()) debugLog("  parth-dl stdout: " + stdOut.left(2000));
    if (!stdErr.isEmpty()) debugLog("  parth-dl stderr: " + stdErr.left(1000));

    proc->deleteLater();

    QFileInfo prefixInfo(job.outPrefix);
    QStringList matches = QDir(prefixInfo.absolutePath())
            .entryList(QStringList() << (prefixInfo.fileName() + "_*"), QDir::Files, QDir::Name);
    debugLog(QString("  parth-dl output matches: %1").arg(matches.join(", ")));

    // Filename -> 1-based slide index, to pair each media file back up with
    // its own "<prefix>_NN.dims" sidecar (see kInstagramFetchScript's doc
    // comment) for real width/height, letting QML size the video surface
    // correctly instead of always falling back to a fixed square.
    QRegExp indexRe(QRegExp::escape(prefixInfo.fileName()) + "_(\\d+)\\..*");

    QVariantList items;
    if (succeeded) {
        foreach (const QString &name, matches) {
            if (name.endsWith(".dims")) continue; // read by index below, not a media file itself
            QString mime = mimeTypeForFile(name);
            if (!mime.startsWith("image/") && !mime.startsWith("video/")) continue; // skip unknown leftovers
            QVariantMap item;
            item["type"] = mime.startsWith("video/") ? "video" : "image";
            item["url"] = "file://" + prefixInfo.absolutePath() + "/" + name;
            if (indexRe.exactMatch(name)) {
                QFile dimsFile(job.outPrefix + "_" + indexRe.cap(1) + ".dims");
                if (dimsFile.open(QIODevice::ReadOnly)) {
                    QStringList wh = QString::fromUtf8(dimsFile.readAll()).trimmed().split(' ');
                    if (wh.size() == 2 && wh.at(0).toInt() > 0 && wh.at(1).toInt() > 0) {
                        item["width"] = wh.at(0).toInt();
                        item["height"] = wh.at(1).toInt();
                    }
                }
            }
            items.append(item);
        }
        // .dims sidecars are consumed above; they never belong in the
        // cached manifest or get treated as a media item themselves.
        foreach (const QString &name, matches) {
            if (name.endsWith(".dims")) QFile::remove(prefixInfo.absolutePath() + "/" + name);
        }
    }

    if (job.isCarousel) {
        QString baseUrl = carouselBaseUrl(job.instagramUrl);
        m_inFlight.remove("carousel|" + baseUrl);
        if (items.isEmpty()) {
            // Nothing usable is going into the manifest, so nothing
            // produced here should linger on disk either.
            foreach (const QString &name, matches) QFile::remove(prefixInfo.absolutePath() + "/" + name);
            emit instagramCarouselFailed(job.instagramUrl);
            return;
        }
        saveCarouselManifest(baseUrl, items);
        emit instagramCarouselReady(job.instagramUrl, items);
        return;
    }

    m_inFlight.remove(job.instagramUrl);
    if (items.isEmpty()) {
        foreach (const QString &name, matches) QFile::remove(prefixInfo.absolutePath() + "/" + name);
        emit instagramVideoFailed(job.instagramUrl);
        return;
    }
    // A single Reel/video fetch has exactly one cache path (see
    // cachePathFor()), unlike a carousel's per-item manifest -- copy the
    // one downloaded file there rather than keeping its "<hash>_dl_01.<ext>"
    // name.
    QString srcPath = items.first().toMap().value("url").toString();
    srcPath.remove("file://");
    QString cachePath = cachePathFor(job.instagramUrl);
    bool ok = QFile::copy(srcPath, cachePath);
    foreach (const QString &name, matches) QFile::remove(prefixInfo.absolutePath() + "/" + name);
    if (!ok) {
        emit instagramVideoFailed(job.instagramUrl);
        return;
    }
    emit instagramVideoReady(job.instagramUrl, "file://" + cachePath);
}

void MediaManager::openVideoExternally(const QString &localFileUrl)
{
    if (localFileUrl.isEmpty()) return;
    // No setTarget() call -- an unbound invoke lets the system resolve the
    // best-registered handler for this action/mimeType (the built-in Videos
    // app), the same "open with the default app" behavior as tapping a
    // video attachment in the system email/BBM apps.
    bb::system::InvokeRequest request;
    request.setAction("bb.action.VIEW");
    request.setMimeType("video/mp4");
    request.setUri(QUrl(localFileUrl));
    m_invokeManager->invoke(request);
}

QString MediaManager::resolveThumbnail(const QString &mxcUri, int width, int height)
{
    if (mxcUri.isEmpty()) return QString();

    QString cachePath = cachePathForThumbnail(mxcUri, width, height);
    if (QFile::exists(cachePath)) {
        return "file://" + cachePath;
    }
    if (m_thumbInFlight.contains(cachePath)) return QString();

    QString server, mediaId;
    if (!splitMxc(mxcUri, &server, &mediaId)) return QString();

    QString downloadPath = QString("/_matrix/media/r0/thumbnail/%1/%2").arg(server, mediaId);
    QVariantMap query;
    query["width"] = QString::number(width);
    query["height"] = QString::number(height);
    query["method"] = "crop";
    QNetworkReply *reply = m_api->rawGet(downloadPath, query);
    m_thumbInFlight.insert(cachePath);
    m_thumbDownloadTarget[reply] = mxcUri;
    m_thumbCachePath[reply] = cachePath;
    connect(reply, SIGNAL(finished()), this, SLOT(onThumbnailDownloadFinished()));
    return QString();
}

void MediaManager::onThumbnailDownloadFinished()
{
    QNetworkReply *reply = qobject_cast<QNetworkReply*>(sender());
    if (!reply) return;
    QString mxcUri = m_thumbDownloadTarget.take(reply);
    QString cachePath = m_thumbCachePath.take(reply);
    m_thumbInFlight.remove(cachePath);

    if (reply->error() != QNetworkReply::NoError) {
        reply->deleteLater();
        emit thumbnailFailed(mxcUri);
        return;
    }

    QByteArray data = reply->readAll();
    reply->deleteLater();

    QFile file(cachePath);
    if (!file.open(QIODevice::WriteOnly)) {
        emit thumbnailFailed(mxcUri);
        return;
    }
    file.write(data);
    file.close();

    emit thumbnailReady(mxcUri, "file://" + cachePath);
}

QString MediaManager::newRecordingPath()
{
    // Plain filesystem path, matching what upload()/QFile expect (and what
    // FilePicker's selectedFiles already hands sendImage() for photos) --
    // callers setting bb.multimedia.AudioRecorder::outputUrl (a QUrl) need
    // to prefix "file://" themselves.
    return QString("%1/recording_%2.m4a").arg(m_cacheDir).arg(++m_recordingCounter);
}

void MediaManager::debugLog(const QString &line)
{
    // bbportLog() writes to the same shared/misc file this used to write
    // directly (still readable via Term49/BerryCore with no PC round-trip)
    // AND to qDebug(), so these lines -- parth-dl's Instagram fetch command/
    // exit code/stdout/stderr in particular -- also show up live in
    // Momentics' console instead of only being visible via the file.
    bbportLog(line);
}

QString MediaManager::mimeTypeForFile(const QString &path) const
{
    QString ext = QFileInfo(path).suffix().toLower();
    if (ext == "jpg" || ext == "jpeg") return "image/jpeg";
    if (ext == "png") return "image/png";
    if (ext == "gif") return "image/gif";
    if (ext == "mp4") return "video/mp4";
    if (ext == "3gp") return "video/3gpp";
    if (ext == "amr") return "audio/amr";
    if (ext == "mp3") return "audio/mpeg";
    if (ext == "m4a") return "audio/mp4";
    if (ext == "ogg" || ext == "opus") return "audio/ogg";
    if (ext == "pdf") return "application/pdf";
    if (ext == "doc") return "application/msword";
    if (ext == "docx") return "application/vnd.openxmlformats-officedocument.wordprocessingml.document";
    if (ext == "xls") return "application/vnd.ms-excel";
    if (ext == "xlsx") return "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet";
    if (ext == "txt") return "text/plain";
    if (ext == "zip") return "application/zip";
    return "application/octet-stream";
}

void MediaManager::uploadAudioAsOgg(const QString &localFilePath)
{
    if (localFilePath.isEmpty()) return;

    QString wavPath = localFilePath + ".wav";
    QString outPath = localFilePath + ".ogg";
    QFile::remove(wavPath);
    QFile::remove(outPath);

    // AAC decode + WAV mux only -- both native to ffmpeg, unlike the actual
    // Opus encode this used to ask ffmpeg to do directly (see
    // finishAudioTranscodeJob()'s comment for why that no longer works on
    // this device's BerryCore build). Mono/48kHz here means
    // OggOpusEncoder::encodeFromPcm() downstream needs no resampling logic
    // of its own.
    QStringList args;
    args << "-y" << "-i" << localFilePath
         << "-ar" << "48000" << "-ac" << "1" << "-f" << "wav"
         << wavPath;

    AudioUploadJob job;
    job.originalPath = localFilePath;
    job.outPath = outPath;
    job.wavPath = wavPath;

    QProcess *proc = new QProcess(this);
    proc->setProcessEnvironment(berryCoreEnvironment());
    m_audioUploadJobs[proc] = job;
    connect(proc, SIGNAL(finished(int,QProcess::ExitStatus)), this, SLOT(onAudioTranscodeFinished(int,QProcess::ExitStatus)));
    connect(proc, SIGNAL(error(QProcess::ProcessError)), this, SLOT(onAudioTranscodeError(QProcess::ProcessError)));
    proc->start(QString::fromLatin1(kBerryCoreFfmpeg), args);
}

void MediaManager::onAudioTranscodeFinished(int exitCode, QProcess::ExitStatus exitStatus)
{
    QProcess *proc = qobject_cast<QProcess*>(sender());
    if (!proc || !m_audioUploadJobs.contains(proc)) return;
    finishAudioTranscodeJob(proc, exitStatus == QProcess::NormalExit && exitCode == 0);
}

void MediaManager::onAudioTranscodeError(QProcess::ProcessError error)
{
    Q_UNUSED(error);
    QProcess *proc = qobject_cast<QProcess*>(sender());
    if (!proc || !m_audioUploadJobs.contains(proc)) return;
    finishAudioTranscodeJob(proc, false);
}

// Reads a RIFF/WAVE file (as ffmpeg's own "-f wav" writes it: fmt chunk
// might not immediately precede data, so this walks chunks by id rather
// than assuming fixed offsets) and returns just the "data" chunk's raw
// bytes -- the 16-bit PCM samples OggOpusEncoder::encodeFromPcm() wants.
static bool extractWavPcm(const QByteArray &wav, QByteArray *pcmOut)
{
    if (wav.size() < 12 || !wav.startsWith("RIFF") || wav.mid(8, 4) != "WAVE") return false;
    int pos = 12;
    while (pos + 8 <= wav.size()) {
        QByteArray chunkId = wav.mid(pos, 4);
        quint32 chunkSize = qFromLittleEndian<quint32>(reinterpret_cast<const uchar*>(wav.constData()) + pos + 4);
        int dataStart = pos + 8;
        if (dataStart + int(chunkSize) > wav.size()) break;
        if (chunkId == "data") {
            *pcmOut = wav.mid(dataStart, int(chunkSize));
            return !pcmOut->isEmpty();
        }
        pos = dataStart + int(chunkSize) + (chunkSize % 2); // chunks are word-aligned
    }
    return false;
}

// Falls back to uploading the original (untranscoded) recording if either
// step fails -- ffmpeg's AAC decode, or (new) the native Opus encode below
// -- same "never block the send outright" philosophy as finishFfmpegJob()
// for incoming video. The recipient still gets a playable file, just as
// plain audio/mp4 (no inline voice-bubble rendering) instead of a proper
// Ogg/Opus voice message.
void MediaManager::finishAudioTranscodeJob(QProcess *proc, bool succeeded)
{
    AudioUploadJob job = m_audioUploadJobs.take(proc);

    debugLog(QString("audio transcode finished path=%1 succeeded=%2 exitCode=%3")
                 .arg(job.originalPath).arg(succeeded).arg(proc->exitCode()));
    QString stdErr = QString::fromUtf8(proc->readAllStandardError());
    if (!stdErr.isEmpty()) debugLog("  ffmpeg stderr: " + stdErr.left(1500));

    proc->deleteLater();

    bool haveOgg = false;
    if (succeeded && QFile::exists(job.wavPath)) {
        QFile wavFile(job.wavPath);
        QByteArray pcm;
        if (wavFile.open(QIODevice::ReadOnly) && extractWavPcm(wavFile.readAll(), &pcm)) {
            QByteArray oggBytes;
            if (OggOpusEncoder::encodeFromPcm(pcm, 1, &oggBytes)) {
                QFile outFile(job.outPath);
                if (outFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                    outFile.write(oggBytes);
                    outFile.close();
                    haveOgg = true;
                }
            } else {
                debugLog("  OggOpusEncoder::encodeFromPcm failed");
            }
        } else {
            debugLog("  couldn't read/parse ffmpeg's WAV output");
        }
    }
    QFile::remove(job.wavPath);

    if (!haveOgg) debugLog("  falling back to untranscoded m4a upload");
    QString uploadPath = haveOgg ? job.outPath : job.originalPath;
    if (haveOgg) m_uploadPathRemap[uploadPath] = job.originalPath;
    else QFile::remove(job.outPath);
    upload(uploadPath);
}

void MediaManager::upload(const QString &localFilePath)
{
    QFile file(localFilePath);
    if (!file.open(QIODevice::ReadOnly)) {
        emit uploadFinished(localFilePath, QString(), QString(), false);
        return;
    }
    QByteArray data = file.readAll();
    file.close();

    QString mimeType = mimeTypeForFile(localFilePath);
    QString fileName = QFileInfo(localFilePath).fileName();
    QString path = QString("/_matrix/media/r0/upload?filename=%1")
            .arg(QString(QUrl::toPercentEncoding(fileName)));

    QNetworkReply *reply = m_api->apiPostRaw(path, data, mimeType);
    m_uploadSourcePath[reply] = localFilePath;
    m_uploadMimeType[reply] = mimeType;
    connect(reply, SIGNAL(finished()), this, SLOT(onUploadFinished()));
}

void MediaManager::onUploadFinished()
{
    QNetworkReply *reply = qobject_cast<QNetworkReply*>(sender());
    if (!reply) return;
    QString localFilePath = m_uploadSourcePath.take(reply);
    QString mimeType = m_uploadMimeType.take(reply);

    bool ok = false;
    QVariant parsed = MatrixApi::parseJson(reply, &ok);
    reply->deleteLater();

    QVariantMap map = parsed.toMap();
    QString mxcUri = map.value("content_uri").toString();

    // uploadAudioAsOgg() uploads a throwaway "<original>.ogg" temp file, not
    // the path the caller actually asked to send -- report the original
    // path instead (so MessageListModel's m_pendingUploads lookup, keyed by
    // what it originally called sendAudio() with, still matches) and clean
    // the temp file up now that it has served its purpose either way.
    QString remapOriginal = m_uploadPathRemap.take(localFilePath);
    if (!remapOriginal.isEmpty()) {
        QFile::remove(localFilePath);
        localFilePath = remapOriginal;
    }

    if (!ok || mxcUri.isEmpty()) {
        emit uploadFinished(localFilePath, QString(), mimeType, false);
        return;
    }
    emit uploadFinished(localFilePath, mxcUri, mimeType, true);
}
