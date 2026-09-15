#include "mediamanager.hpp"
#include "matrixapi.hpp"
#include "oggopusdecoder.hpp"
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
#include <cstdlib>

#include <bb/system/InvokeManager>
#include <bb/system/InvokeRequest>

using namespace bb::data;

// Video transcoding and Instagram Reel extraction used to be delegated to a
// dev-machine PC proxy script (long since removed); both features moved
// on-device using BerryCore's (github.com/sw7ft/BerryCore) bundled
// ffmpeg/yt-dlp binaries instead, invoked directly via QProcess. No PC
// involvement needed any more.
static const char *kBerryCoreRoot = "/accounts/1000/shared/misc/berrycore";
static const char *kBerryCoreFfmpeg = "/accounts/1000/shared/misc/berrycore/bin/ffmpeg";
static const char *kBerryCorePython3 = "/accounts/1000/shared/misc/berrycore/bin/python3";

// yt-dlp can't download a mixed Instagram carousel's photo entries at all
// (see the doc comment above the "?img_index=N" / shortcode dead end in
// finishCarouselJob()), and scraping the plain post page's HTML for an
// embedded "display_url" JSON blob (an earlier attempt) turned out to be a
// dead end too -- confirmed on a real device that anonymous requests get a
// full, legitimate 600KB+ page with no such JSON at all any more (modern
// Instagram loads post data client-side via JS after the initial render,
// not server-embedded).
//
// What actually still works anonymously: Instagram's own web client calls
// a legacy GraphQL endpoint by shortcode, identified by a fixed doc_id.
// This is the same endpoint/header pair used by other no-login Instagram
// scrapers (e.g. parth-dl's extractors.py: X-IG-App-ID 936619743392459).
// A first attempt using only that header got HTTP 401 on a real device --
// Instagram's anonymous/"guest" access to this endpoint also checks for
// the same csrftoken/mid/ig_did cookies a real browser picks up on its
// very first page visit, plus an X-CSRFToken header matching that cookie
// (the standard anti-CSRF pattern, enforced here even though this is a
// GET). So this primes an anonymous session by fetching the plain post
// page first (through a cookie jar), then reuses those cookies plus the
// resulting csrftoken for the actual GraphQL call. The response is real
// JSON with each sidecar child's "display_url" directly in
// edge_sidecar_to_children, unrelated to yt-dlp's broken photo-format
// walk entirely.
//
// Invoked as `python3 -c <this> <postUrl> <outPrefix> <comma-separated
// 1-based missing indices>`; writes each found image to
// "<outPrefix><NN>.jpg" and prints one "OK N url" / "FAIL N reason" /
// "MISSING_INDEX N" line per requested index for debugLog to capture.
static const char *kCarouselPhotoScrapeScript =
    "import sys, re, json, ssl, http.cookiejar, urllib.request, urllib.parse\n"
    "\n"
    "url, out_prefix, missing_str = sys.argv[1], sys.argv[2], sys.argv[3]\n"
    "missing = [int(x) for x in missing_str.split(',') if x]\n"
    "\n"
    "m = re.search(r'/p/([^/?]+)', url)\n"
    "shortcode = m.group(1) if m else ''\n"
    "post_url = 'https://www.instagram.com/p/%s/' % shortcode\n"
    "\n"
    "ctx = ssl.create_default_context()\n"
    "ctx.check_hostname = False\n"
    "ctx.verify_mode = ssl.CERT_NONE\n"
    "\n"
    "ua = 'Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/124.0 Safari/537.36'\n"
    "cj = http.cookiejar.CookieJar()\n"
    "opener = urllib.request.build_opener(urllib.request.HTTPSHandler(context=ctx), urllib.request.HTTPCookieProcessor(cj))\n"
    "\n"
    "ordered = []\n"
    "try:\n"
    "    prime = urllib.request.Request(post_url, headers={'User-Agent': ua, 'Accept-Language': 'en-US,en;q=0.9'})\n"
    "    opener.open(prime, timeout=20).read()\n"
    "    csrftoken = ''\n"
    "    for c in cj:\n"
    "        if c.name == 'csrftoken':\n"
    "            csrftoken = c.value\n"
    "    print('primed cookies=%d csrftoken=%s' % (len(list(cj)), bool(csrftoken)))\n"
    "\n"
    "    variables = json.dumps({'shortcode': shortcode})\n"
    "    gql_url = 'https://www.instagram.com/graphql/query/?doc_id=8845758582119845&variables=' + urllib.parse.quote(variables)\n"
    "    headers = {\n"
    "        'User-Agent': ua,\n"
    "        'X-IG-App-ID': '936619743392459',\n"
    "        'X-CSRFToken': csrftoken,\n"
    "        'X-Requested-With': 'XMLHttpRequest',\n"
    "        'Accept': '*/*',\n"
    "        'Referer': post_url,\n"
    "    }\n"
    "    req = urllib.request.Request(gql_url, headers=headers)\n"
    "    resp = opener.open(req, timeout=20)\n"
    "    raw = resp.read()\n"
    "    print('gql fetched status=%s len=%d' % (resp.getcode(), len(raw)))\n"
    "    data = json.loads(raw.decode('utf-8', 'ignore'))\n"
    "    media = (data.get('data') or {}).get('xdt_shortcode_media') or {}\n"
    "    edges = ((media.get('edge_sidecar_to_children') or {}).get('edges')) or []\n"
    "    print('edges=%d' % len(edges))\n"
    "    for e in edges:\n"
    "        node = e.get('node') or {}\n"
    "        ordered.append(node.get('display_url'))\n"
    "    if not edges and media.get('display_url'):\n"
    "        ordered.append(media.get('display_url'))\n"
    "except Exception as e:\n"
    "    print('GQL_FAILED %s' % e)\n"
    "\n"
    "print('found %d display_url candidate(s)' % len([u for u in ordered if u]))\n"
    "\n"
    "for idx in missing:\n"
    "    if idx - 1 >= len(ordered) or not ordered[idx - 1]:\n"
    "        print('MISSING_INDEX %d' % idx)\n"
    "        continue\n"
    "    img_url = ordered[idx - 1]\n"
    "    try:\n"
    "        data2 = urllib.request.urlopen(urllib.request.Request(img_url, headers={'User-Agent': ua}), context=ctx, timeout=20).read()\n"
    "        with open('%s%02d.jpg' % (out_prefix, idx), 'wb') as f:\n"
    "            f.write(data2)\n"
    "        print('OK %d %s' % (idx, img_url))\n"
    "    except Exception as e:\n"
    "        print('FAIL %d %s' % (idx, e))\n";

// BerryCore's bundled youtube-dl binary (not this) hits an on-device-
// confirmed bug: every HTTPS request fails with "tlsv1 alert protocol
// version" (OpenSSL itself is a modern 3.3.2 -- confirmed via `python3 -c
// "import ssl; print(ssl.OPENSSL_VERSION)"` -- so this is youtube-dl's own
// long-unmaintained request code pinning an obsolete TLS version, not a
// BerryCore/OpenSSL limitation). yt-dlp is the actively-maintained fork
// that doesn't have that bug, already available via `pip install yt-dlp`
// on this Python -- hence "python3 -m yt_dlp" rather than the youtube-dl
// binary BerryCore ships.
//
// ffmpeg also needs BerryCore's LD_LIBRARY_PATH (built against its bundled
// QNX target tree, not BBNDK's). Mirrors berrycore/env.sh exactly.
static QProcessEnvironment berryCoreEnvironment()
{
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    QString root = QString::fromLatin1(kBerryCoreRoot);
    QString path = root + "/bin:" + root + "/sbin:" + env.value("PATH");
    QString ldLibraryPath = root + "/target_10_3_1_995/qnx6/armle-v7/usr/lib:" + root + "/lib:" + env.value("LD_LIBRARY_PATH");
    env.insert("PATH", path);
    env.insert("LD_LIBRARY_PATH", ldLibraryPath);
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

    // yt-dlp substitutes %(ext)s with the real container extension
    // (mp4 for essentially every Instagram Reel); the exact resulting
    // filename is located afterwards in finishYoutubeDlJob() via a glob.
    QString outTemplate = cachePath + "_dl.%(ext)s";

    // Clear out any leftover from a previous run that never made it to
    // cleanup (e.g. the app was killed between yt-dlp finishing and
    // finishYoutubeDlJob()'s QFile::remove() calls) -- otherwise that
    // glob could match the stale file instead of (or alongside) the one
    // this run is about to produce, serving old/wrong content for this URL.
    {
        QFileInfo templateInfo(outTemplate);
        QString namePrefix = templateInfo.completeBaseName();
        QStringList leftovers = QDir(templateInfo.absolutePath())
                .entryList(QStringList() << (namePrefix + ".*"), QDir::Files);
        foreach (const QString &name, leftovers) {
            QFile::remove(templateInfo.absolutePath() + "/" + name);
        }
    }

    QStringList args;
    args << "-m" << "yt_dlp"
         << "--no-warnings" << "--no-check-certificate"
         << "--ffmpeg-location" << QString::fromLatin1(kBerryCoreFfmpeg)
         << "-f" << "best"
         << "-o" << outTemplate
         << instagramUrl;

    debugLog("yt-dlp starting: " + QString::fromLatin1(kBerryCorePython3) + " " + args.join(" "));

    QProcess *proc = new QProcess(this);
    proc->setProcessEnvironment(berryCoreEnvironment());
    m_ytdlTarget[proc] = instagramUrl;
    m_ytdlOutTemplate[proc] = outTemplate;
    connect(proc, SIGNAL(finished(int,QProcess::ExitStatus)), this, SLOT(onYoutubeDlFinished(int,QProcess::ExitStatus)));
    connect(proc, SIGNAL(error(QProcess::ProcessError)), this, SLOT(onYoutubeDlError(QProcess::ProcessError)));
    proc->start(QString::fromLatin1(kBerryCorePython3), args);
    return QString();
}

void MediaManager::onYoutubeDlFinished(int exitCode, QProcess::ExitStatus exitStatus)
{
    QProcess *proc = qobject_cast<QProcess*>(sender());
    if (!proc || !m_ytdlTarget.contains(proc)) return; // error() already handled it
    finishYoutubeDlJob(proc, exitStatus == QProcess::NormalExit && exitCode == 0);
}

void MediaManager::onYoutubeDlError(QProcess::ProcessError error)
{
    QProcess *proc = qobject_cast<QProcess*>(sender());
    if (!proc || !m_ytdlTarget.contains(proc)) return;
    debugLog(QString("yt-dlp process error: %1 (%2)").arg(int(error)).arg(proc->errorString()));
    finishYoutubeDlJob(proc, false);
}

void MediaManager::finishYoutubeDlJob(QProcess *proc, bool succeeded)
{
    QString instagramUrl = m_ytdlTarget.take(proc);
    QString outTemplate = m_ytdlOutTemplate.take(proc);

    // Temporary diagnostic logging (Reel fetch bring-up) -- yt-dlp's own
    // stdout/stderr is the only way to see WHY it failed (unsupported URL,
    // extractor error, network issue, missing Python module, ...).
    debugLog(QString("yt-dlp finished url=%1 succeeded=%2 exitCode=%3")
                 .arg(instagramUrl).arg(succeeded).arg(proc->exitCode()));
    QString stdOut = QString::fromUtf8(proc->readAllStandardOutput());
    QString stdErr = QString::fromUtf8(proc->readAllStandardError());
    if (!stdOut.isEmpty()) debugLog("  yt-dlp stdout: " + stdOut.left(2000));
    if (!stdErr.isEmpty()) debugLog("  yt-dlp stderr: " + stdErr.left(2000));

    proc->deleteLater();
    m_inFlight.remove(instagramUrl);

    // outTemplate is "<hash>_dl.%(ext)s" -- glob for whatever extension
    // yt-dlp actually picked.
    QFileInfo templateInfo(outTemplate);
    QString namePrefix = templateInfo.completeBaseName(); // "<hash>_dl"
    QStringList matches = QDir(templateInfo.absolutePath())
            .entryList(QStringList() << (namePrefix + ".*"), QDir::Files);
    debugLog(QString("  yt-dlp output matches: %1").arg(matches.join(", ")));

    QString cachePath = cachePathFor(instagramUrl);
    bool ok = false;
    if (succeeded && !matches.isEmpty()) {
        QString downloadedPath = templateInfo.absolutePath() + "/" + matches.first();
        ok = QFile::copy(downloadedPath, cachePath);
        QFile::remove(downloadedPath);
    }
    // Clean up any other leftover candidates (e.g. a partial .part file).
    foreach (const QString &name, matches) {
        QFile::remove(templateInfo.absolutePath() + "/" + name);
    }

    if (!ok) {
        emit instagramVideoFailed(instagramUrl);
        return;
    }
    emit instagramVideoReady(instagramUrl, "file://" + cachePath);
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

    // One file per carousel slide via yt-dlp's own playlist support --
    // Instagram carousels extract as a multi-entry playlist (confirmed via
    // yt-dlp's own issue tracker discussing mixed image/video carousels).
    // playlist_index is zero-padded (%02d) so alphabetical globbing below
    // (QDir::Name) sorts slide 10 after slide 9, not before slide 2.
    QString outPrefix = carouselCacheKey(baseUrl) + "_dl";
    QString outTemplate = outPrefix + "_%(playlist_index)02d.%(ext)s";

    // Same stale-leftover cleanup as fetchInstagramVideo(), scoped to this
    // carousel's own prefix -- otherwise a previous run's partial files
    // could get globbed alongside (or instead of) this run's real output.
    {
        QFileInfo prefixInfo(outPrefix);
        QStringList leftovers = QDir(prefixInfo.absolutePath())
                .entryList(QStringList() << (prefixInfo.fileName() + "_*"), QDir::Files);
        foreach (const QString &name, leftovers) {
            QFile::remove(prefixInfo.absolutePath() + "/" + name);
        }
    }

    // No "-f best": that's fine for a single Reel's video, but forcing a
    // video-oriented format selector onto a carousel mixing image and video
    // entries is exactly the kind of thing yt-dlp's own issue tracker flags
    // as fragile -- leaving format selection to yt-dlp's own per-entry
    // defaults (already how a plain single Instagram photo post downloads
    // correctly) is safer across a mixed carousel.
    QStringList args;
    args << "-m" << "yt_dlp"
         << "--no-warnings" << "--no-check-certificate"
         << "--ffmpeg-location" << QString::fromLatin1(kBerryCoreFfmpeg)
         << "-o" << outTemplate
         << baseUrl;

    debugLog("yt-dlp (carousel) starting: " + QString::fromLatin1(kBerryCorePython3) + " " + args.join(" "));

    QProcess *proc = new QProcess(this);
    proc->setProcessEnvironment(berryCoreEnvironment());
    m_carouselTarget[proc] = instagramUrl;
    m_carouselOutPrefix[proc] = outPrefix;
    connect(proc, SIGNAL(finished(int,QProcess::ExitStatus)), this, SLOT(onCarouselYoutubeDlFinished(int,QProcess::ExitStatus)));
    connect(proc, SIGNAL(error(QProcess::ProcessError)), this, SLOT(onCarouselYoutubeDlError(QProcess::ProcessError)));
    proc->start(QString::fromLatin1(kBerryCorePython3), args);

    // See m_carouselProcTimer's doc comment: guarantees finishCarouselJob()
    // always eventually runs even if yt-dlp itself never exits.
    QTimer *watchdog = new QTimer(this);
    watchdog->setSingleShot(true);
    m_carouselProcTimer[proc] = watchdog;
    m_carouselTimeoutTarget[watchdog] = proc;
    connect(watchdog, SIGNAL(timeout()), this, SLOT(onCarouselTimeout()));
    watchdog->start(45000);

    return QVariantList();
}

void MediaManager::onCarouselTimeout()
{
    QTimer *watchdog = qobject_cast<QTimer*>(sender());
    if (!watchdog) return;
    QProcess *proc = m_carouselTimeoutTarget.take(watchdog);
    watchdog->deleteLater();
    if (!proc || !m_carouselTarget.contains(proc)) return; // already finished normally
    m_carouselProcTimer.remove(proc);
    debugLog("yt-dlp (carousel) timed out after 45s -- killing and failing the fetch");
    proc->kill();
    finishCarouselJob(proc, false);
}

void MediaManager::onCarouselYoutubeDlFinished(int exitCode, QProcess::ExitStatus exitStatus)
{
    QProcess *proc = qobject_cast<QProcess*>(sender());
    if (!proc || !m_carouselTarget.contains(proc)) return; // error() already handled it
    // NOT gated on exitCode == 0: confirmed via a real device log that
    // yt-dlp's Instagram extractor exits 1 for an entirely ordinary mixed
    // photo+video carousel -- it tries to pull "video formats" for every
    // playlist entry and errors out per-entry on the photo slides ("No
    // video formats found!"), even though the video slide(s) downloaded
    // completely fine. Gating on exitCode used to discard (and delete) a
    // perfectly good downloaded file just because yt-dlp's own summary
    // exit code reflected those unrelated per-photo failures. NormalExit
    // (not Crashed/killed) is enough to trust whatever files actually
    // landed on disk -- finishCarouselJob() below only accepts entries
    // that pass a real image/video mimetype check anyway.
    finishCarouselJob(proc, exitStatus == QProcess::NormalExit);
}

void MediaManager::onCarouselYoutubeDlError(QProcess::ProcessError error)
{
    QProcess *proc = qobject_cast<QProcess*>(sender());
    if (!proc || !m_carouselTarget.contains(proc)) return;
    debugLog(QString("yt-dlp (carousel) process error: %1 (%2)").arg(int(error)).arg(proc->errorString()));
    finishCarouselJob(proc, false);
}

void MediaManager::finishCarouselJob(QProcess *proc, bool succeeded)
{
    QString instagramUrl = m_carouselTarget.take(proc);
    QString outPrefix = m_carouselOutPrefix.take(proc);
    QString baseUrl = carouselBaseUrl(instagramUrl);

    QTimer *watchdog = m_carouselProcTimer.take(proc);
    if (watchdog) {
        m_carouselTimeoutTarget.remove(watchdog);
        watchdog->stop();
        watchdog->deleteLater();
    }

    // Temporary diagnostic logging (carousel fetch bring-up) -- same
    // reasoning as finishYoutubeDlJob()'s: yt-dlp's own stdout/stderr is the
    // only way to see why it failed.
    debugLog(QString("yt-dlp (carousel) finished url=%1 succeeded=%2 exitCode=%3")
                 .arg(baseUrl).arg(succeeded).arg(proc->exitCode()));
    QString stdOut = QString::fromUtf8(proc->readAllStandardOutput());
    QString stdErr = QString::fromUtf8(proc->readAllStandardError());
    if (!stdOut.isEmpty()) debugLog("  yt-dlp stdout: " + stdOut.left(2000));
    if (!stdErr.isEmpty()) debugLog("  yt-dlp stderr: " + stdErr.left(2000));

    proc->deleteLater();
    m_inFlight.remove("carousel|" + baseUrl);

    QFileInfo prefixInfo(outPrefix);
    QStringList matches = QDir(prefixInfo.absolutePath())
            .entryList(QStringList() << (prefixInfo.fileName() + "_*"), QDir::Files, QDir::Name);
    debugLog(QString("  yt-dlp (carousel) output matches: %1").arg(matches.join(", ")));

    QVariantList items;
    if (succeeded) {
        foreach (const QString &name, matches) {
            QString mime = mimeTypeForFile(name);
            if (!mime.startsWith("image/") && !mime.startsWith("video/")) continue; // skip .part/unknown leftovers
            QVariantMap item;
            item["type"] = mime.startsWith("video/") ? "video" : "image";
            item["url"] = "file://" + prefixInfo.absolutePath() + "/" + name;
            items.append(item);
        }
    }

    if (items.isEmpty()) {
        // Nothing usable is going into the manifest, so nothing produced
        // here should linger on disk either.
        foreach (const QString &name, matches) {
            QFile::remove(prefixInfo.absolutePath() + "/" + name);
        }
        emit instagramCarouselFailed(instagramUrl);
        return;
    }

    // yt-dlp's Instagram extractor tries to resolve "video formats" for
    // every entry of a mixed photo+video carousel and errors out on the
    // photo ones ("No video formats found!") -- a known, maintainer-closed
    // wontfix limitation (github.com/yt-dlp/yt-dlp issue #7569), confirmed
    // at the extractor source level: a photo sidecar child is never given
    // downloadable formats, no matter which URL reaches it. On a real
    // device, both Instagram's own "?img_index=N" link and each child's
    // own shortcode-as-permalink re-triggered the exact same full-sidecar
    // extraction and error set every time -- individual sidecar children
    // have no independently extractable permalink via yt-dlp at all, so
    // retrying with a different URL is a dead end.
    //
    // What actually works: the same post page yt-dlp already downloads
    // (its own "Downloading webpage" step) embeds each sidecar child's
    // direct CDN image URL as a "display_url" field in inline JSON --
    // yt-dlp parses this same JSON internally but only follows it for the
    // video-format walk, discarding it for photo entries. A small,
    // separate Python scrape of that same page's "display_url" fields
    // gets the missing photos directly, with zero dependency on yt-dlp's
    // broken format-resolution path.
    QRegExp countRe("Downloading\\s+\\d+\\s+items?\\s+of\\s+(\\d+)");
    int totalSlides = (countRe.indexIn(stdOut) >= 0) ? countRe.cap(1).toInt() : -1;

    QRegExp indexRe(QRegExp::escape(prefixInfo.fileName()) + "_(\\d+)\\..*");
    QList<int> presentIndices;
    foreach (const QString &name, matches) {
        if (indexRe.exactMatch(name)) presentIndices.append(indexRe.cap(1).toInt());
    }

    QStringList missingIndices;
    // Instagram's own UI caps a carousel at 10 slides -- also guards
    // against a stdout parse going wrong and looping some huge bogus count.
    for (int i = 1; i <= totalSlides && i <= 10; ++i) {
        if (!presentIndices.contains(i)) missingIndices.append(QString::number(i));
    }

    if (missingIndices.isEmpty()) {
        startCarouselVideoEncodes(baseUrl, instagramUrl, items);
        return;
    }

    debugLog(QString("yt-dlp (carousel) slide(s) %1 missing from the playlist run -- scraping display_url directly")
                 .arg(missingIndices.join(",")));

    CarouselSlideFetchPending slidePending;
    slidePending.instagramUrl = instagramUrl;
    slidePending.items = items;
    slidePending.pendingFetches = 1;

    QString slotPrefix = outPrefix + "_photo";

    QStringList args;
    args << "-c" << QString::fromLatin1(kCarouselPhotoScrapeScript)
         << baseUrl << slotPrefix << missingIndices.join(",");

    CarouselSlideFetchJob job;
    job.baseUrl = baseUrl;
    job.slotPrefix = slotPrefix;

    QProcess *slideProc = new QProcess(this);
    slideProc->setProcessEnvironment(berryCoreEnvironment());
    m_carouselSlideJobs[slideProc] = job;
    connect(slideProc, SIGNAL(finished(int,QProcess::ExitStatus)), this, SLOT(onCarouselSlideFetchFinished(int,QProcess::ExitStatus)));
    connect(slideProc, SIGNAL(error(QProcess::ProcessError)), this, SLOT(onCarouselSlideFetchError(QProcess::ProcessError)));
    slideProc->start(QString::fromLatin1(kBerryCorePython3), args);

    m_carouselSlideFetchPending[baseUrl] = slidePending;
}

void MediaManager::onCarouselSlideFetchFinished(int exitCode, QProcess::ExitStatus exitStatus)
{
    QProcess *proc = qobject_cast<QProcess*>(sender());
    if (!proc || !m_carouselSlideJobs.contains(proc)) return;
    finishCarouselSlideFetch(proc, exitStatus == QProcess::NormalExit);
}

void MediaManager::onCarouselSlideFetchError(QProcess::ProcessError error)
{
    QProcess *proc = qobject_cast<QProcess*>(sender());
    if (!proc || !m_carouselSlideJobs.contains(proc)) return;
    debugLog(QString("yt-dlp (carousel) missing-slide fetch process error: %1 (%2)").arg(int(error)).arg(proc->errorString()));
    finishCarouselSlideFetch(proc, false);
}

void MediaManager::finishCarouselSlideFetch(QProcess *proc, bool succeeded)
{
    CarouselSlideFetchJob job = m_carouselSlideJobs.take(proc);

    debugLog(QString("carousel photo scrape finished succeeded=%1 exitCode=%2")
                 .arg(succeeded).arg(proc->exitCode()));
    QString stdOut = QString::fromUtf8(proc->readAllStandardOutput());
    QString stdErr = QString::fromUtf8(proc->readAllStandardError());
    if (!stdOut.isEmpty()) debugLog("  scrape stdout: " + stdOut.left(2000));
    if (!stdErr.isEmpty()) debugLog("  scrape stderr: " + stdErr.left(1000));

    proc->deleteLater();

    QFileInfo slotInfo(job.slotPrefix);
    QStringList found = QDir(slotInfo.absolutePath())
            .entryList(QStringList() << (slotInfo.fileName() + "*"), QDir::Files);

    if (!m_carouselSlideFetchPending.contains(job.baseUrl)) {
        // The carousel this belonged to already finalized/abandoned --
        // clean up whatever this fetch produced rather than leak it.
        foreach (const QString &name, found) QFile::remove(slotInfo.absolutePath() + "/" + name);
        return;
    }

    CarouselSlideFetchPending &pending = m_carouselSlideFetchPending[job.baseUrl];

    if (succeeded) {
        foreach (const QString &name, found) {
            QString mime = mimeTypeForFile(name);
            if (!mime.startsWith("image/") && !mime.startsWith("video/")) continue;
            QVariantMap item;
            item["type"] = mime.startsWith("video/") ? "video" : "image";
            item["url"] = "file://" + slotInfo.absolutePath() + "/" + name;
            pending.items.append(item);
        }
    } else {
        foreach (const QString &name, found) QFile::remove(slotInfo.absolutePath() + "/" + name);
    }

    --pending.pendingFetches;
    finalizeCarouselSlideFetchesIfDone(job.baseUrl);
}

void MediaManager::finalizeCarouselSlideFetchesIfDone(const QString &baseUrl)
{
    if (!m_carouselSlideFetchPending.contains(baseUrl)) return;
    CarouselSlideFetchPending pending = m_carouselSlideFetchPending.value(baseUrl);
    if (pending.pendingFetches > 0) return;
    m_carouselSlideFetchPending.remove(baseUrl);

    if (pending.items.isEmpty()) {
        emit instagramCarouselFailed(pending.instagramUrl);
        return;
    }

    startCarouselVideoEncodes(baseUrl, pending.instagramUrl, pending.items);
}

void MediaManager::startCarouselVideoEncodes(const QString &baseUrl, const QString &instagramUrl, const QVariantList &items)
{
    // Video items are muxed at Instagram's own export resolution/profile
    // (yt-dlp's own ffmpeg just combines the separate DASH video+audio
    // streams into one container) -- same "audio fine, video stays black"
    // Q5 hardware-decoder limitation as regular video messages, so each
    // needs the identical re-encode resolve()'s isVideo branch already
    // does before it's actually playable. Kicked off here as its own
    // fan-out/fan-in stage; instagramCarouselReady/Failed only fires once
    // every video item's encode (if any) has landed -- see
    // finalizeCarouselIfDone().
    CarouselPending pending;
    pending.instagramUrl = instagramUrl;
    pending.items = items;
    pending.pendingEncodes = 0;

    for (int i = 0; i < items.size(); ++i) {
        QVariantMap item = items.at(i).toMap();
        if (item.value("type").toString() != "video") continue;

        QString inPath = item.value("url").toString();
        inPath.remove("file://");
        QString outPath = inPath + "_enc.mp4";

        QStringList args;
        args << "-y" << "-i" << inPath
             << "-vf" << "scale='min(1280,iw)':-2"
             << "-c:v" << "libx264" << "-profile:v" << "high" << "-level" << "4.0"
             << "-b:v" << "2500k" << "-maxrate" << "2500k" << "-bufsize" << "5000k"
             << "-c:a" << "aac" << "-b:a" << "128k" << "-ar" << "44100"
             << "-movflags" << "+faststart"
             << outPath;

        debugLog("yt-dlp (carousel) re-encoding video slide: " + inPath);

        CarouselVideoEncodeJob job;
        job.baseUrl = baseUrl;
        job.inPath = inPath;
        job.outPath = outPath;
        job.itemIndex = i;

        QProcess *encProc = new QProcess(this);
        encProc->setProcessEnvironment(berryCoreEnvironment());
        m_carouselEncodeJobs[encProc] = job;
        connect(encProc, SIGNAL(finished(int,QProcess::ExitStatus)), this, SLOT(onCarouselEncodeFinished(int,QProcess::ExitStatus)));
        connect(encProc, SIGNAL(error(QProcess::ProcessError)), this, SLOT(onCarouselEncodeError(QProcess::ProcessError)));
        encProc->start(QString::fromLatin1(kBerryCoreFfmpeg), args);
        ++pending.pendingEncodes;
    }

    m_carouselPending[baseUrl] = pending;
    finalizeCarouselIfDone(baseUrl);
}

void MediaManager::onCarouselEncodeFinished(int exitCode, QProcess::ExitStatus exitStatus)
{
    QProcess *proc = qobject_cast<QProcess*>(sender());
    if (!proc || !m_carouselEncodeJobs.contains(proc)) return;
    finishCarouselEncodeJob(proc, exitStatus == QProcess::NormalExit && exitCode == 0);
}

void MediaManager::onCarouselEncodeError(QProcess::ProcessError error)
{
    QProcess *proc = qobject_cast<QProcess*>(sender());
    if (!proc || !m_carouselEncodeJobs.contains(proc)) return;
    debugLog(QString("yt-dlp (carousel) re-encode process error: %1 (%2)").arg(int(error)).arg(proc->errorString()));
    finishCarouselEncodeJob(proc, false);
}

void MediaManager::finishCarouselEncodeJob(QProcess *proc, bool succeeded)
{
    CarouselVideoEncodeJob job = m_carouselEncodeJobs.take(proc);
    if (!succeeded) {
        debugLog(QString("yt-dlp (carousel) ffmpeg re-encode failed exitCode=%1 crashed=%2 inSize=%3: %4")
                     .arg(proc->exitCode())
                     .arg(proc->exitStatus() == QProcess::CrashExit)
                     .arg(QFileInfo(job.inPath).size())
                     .arg(job.inPath));
        QString ffmpegOut = QString::fromUtf8(proc->readAllStandardOutput());
        QString ffmpegErr = QString::fromUtf8(proc->readAllStandardError());
        // The on-device console view truncates any single long line, which
        // was hiding ffmpeg's actual error behind its own (large, boring)
        // version/configuration banner every time -- split on real
        // newlines and log only the tail as short, separate lines so the
        // part that actually matters survives console truncation.
        if (!ffmpegErr.isEmpty()) {
            QStringList errLines = ffmpegErr.split('\n', QString::SkipEmptyParts);
            int start = qMax(0, errLines.size() - 10);
            debugLog(QString("  ffmpeg stderr: %1 line(s) total, showing last %2")
                         .arg(errLines.size()).arg(errLines.size() - start));
            for (int i = start; i < errLines.size(); ++i) {
                debugLog(QString("  ffmpeg[%1]: %2").arg(i).arg(errLines.at(i).left(300)));
            }
        } else {
            debugLog("  ffmpeg stderr: (empty)");
        }
        if (!ffmpegOut.isEmpty()) debugLog("  ffmpeg stdout: " + ffmpegOut.right(500));

        // One-time diagnostic: libx264 turned out to be missing from this
        // BerryCore ffmpeg build entirely (see the "Unknown encoder" line
        // above) -- rather than guess at another codec name blind, list
        // what this exact binary actually has compiled in, once, so the
        // next real-device test settles it instead of another round trip.
        static bool probedEncoders = false;
        if (!probedEncoders) {
            probedEncoders = true;
            QProcess probe;
            probe.setProcessEnvironment(berryCoreEnvironment());
            probe.start(QString::fromLatin1(kBerryCoreFfmpeg), QStringList() << "-hide_banner" << "-encoders");
            if (probe.waitForFinished(5000)) {
                QString out = QString::fromUtf8(probe.readAllStandardOutput());
                QStringList lines = out.split('\n', QString::SkipEmptyParts);
                debugLog(QString("ffmpeg -encoders: %1 line(s) total, filtering for video codecs").arg(lines.size()));
                foreach (const QString &line, lines) {
                    QString lower = line.toLower();
                    if (lower.contains("264") || lower.contains("265") || lower.contains("hevc")
                        || lower.contains("vp8") || lower.contains("vp9") || lower.contains("av1")
                        || lower.contains("mpeg4") || lower.contains("theora")) {
                        debugLog("  encoder: " + line.trimmed().left(150));
                    }
                }
            } else {
                debugLog("  ffmpeg -encoders probe timed out");
                probe.kill();
            }
        }
    }
    proc->deleteLater();

    if (!m_carouselPending.contains(job.baseUrl)) {
        // The carousel this belonged to already finalized (shouldn't
        // normally happen, since finalizeCarouselIfDone() only fires once
        // every encode -- including this one -- has landed, but clean up
        // regardless rather than leaking a stray output file).
        QFile::remove(job.outPath);
        return;
    }

    CarouselPending &pending = m_carouselPending[job.baseUrl];
    QVariantMap item = pending.items.at(job.itemIndex).toMap();
    if (succeeded && QFile::exists(job.outPath)) {
        item["url"] = "file://" + job.outPath;
        QFile::remove(job.inPath); // raw muxed file is no longer needed
    } else {
        // Same fallback as finishFfmpegJob() for regular video messages:
        // NativeVideoPlayer's mm-renderer wrapper plays arbitrary H.264/etc.
        // content directly, so the untranscoded original is still usable --
        // confirmed necessary on a real device where this ffmpeg build has
        // no libx264 encoder at all ("Unknown encoder 'libx264'"), meaning
        // the encode step can never succeed here regardless of args.
        debugLog("yt-dlp (carousel) re-encode failed, using original file: " + job.inPath);
        item["url"] = "file://" + job.inPath;
        QFile::remove(job.outPath);
    }
    pending.items.replace(job.itemIndex, item);
    --pending.pendingEncodes;

    finalizeCarouselIfDone(job.baseUrl);
}

void MediaManager::finalizeCarouselIfDone(const QString &baseUrl)
{
    if (!m_carouselPending.contains(baseUrl)) return;
    CarouselPending pending = m_carouselPending.value(baseUrl);
    if (pending.pendingEncodes > 0) return;
    m_carouselPending.remove(baseUrl);

    if (pending.items.isEmpty()) {
        emit instagramCarouselFailed(pending.instagramUrl);
        return;
    }

    saveCarouselManifest(baseUrl, pending.items);
    emit instagramCarouselReady(pending.instagramUrl, pending.items);
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
    // AND to qDebug(), so these lines -- yt-dlp's carousel command/exit
    // code/stdout/stderr in particular -- also show up live in Momentics'
    // console instead of only being visible via the file.
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

    QString outPath = localFilePath + ".ogg";
    QFile::remove(outPath);

    QStringList args;
    args << "-y" << "-i" << localFilePath
         << "-c:a" << "libopus" << "-b:a" << "32k" << "-vbr" << "on" << "-application" << "voip"
         << outPath;

    AudioUploadJob job;
    job.originalPath = localFilePath;
    job.outPath = outPath;

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

// Falls back to uploading the original (untranscoded) recording on any
// ffmpeg failure -- same "never block the send outright" philosophy as
// finishFfmpegJob() for incoming video. The recipient still gets a playable
// file, just as plain audio/mp4 (no inline voice-bubble rendering) instead
// of a proper Ogg/Opus voice message.
void MediaManager::finishAudioTranscodeJob(QProcess *proc, bool succeeded)
{
    AudioUploadJob job = m_audioUploadJobs.take(proc);
    proc->deleteLater();

    bool haveOgg = succeeded && QFile::exists(job.outPath);
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
