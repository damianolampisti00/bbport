#ifndef MEDIAMANAGER_HPP_
#define MEDIAMANAGER_HPP_

#include <QObject>
#include <QString>
#include <QHash>
#include <QSet>
#include <QVariantMap>
#include <QVariantList>
#include <QProcess>
#include <QTimer>

namespace bb { namespace system { class InvokeManager; } }

class MatrixApi;
class QNetworkReply;

// Downloads and caches mxc:// media to local disk so QML ImageView can show
// it, and uploads local files back to the homeserver for outgoing media
// messages.
class MediaManager : public QObject
{
    Q_OBJECT

public:
    explicit MediaManager(MatrixApi *api, QObject *parent = 0);
    virtual ~MediaManager();

    // Returns a "file://" path if already cached; otherwise starts an async
    // download (mediaReady() fires later) and returns an empty string. When
    // key/iv are non-empty (an E2EE room's "file" object: key.k, iv,
    // hashes.sha256), the downloaded ciphertext is AES-256-CTR decrypted
    // before being cached, so the cached file is always plaintext -- callers
    // (QML's ImageView, etc.) never need to know whether the source room was
    // encrypted.
    // isVideo routes the downloaded (and decrypted, if applicable) bytes
    // through BerryCore's on-device ffmpeg binary before caching -- the Q5's
    // hardware video decoder can't handle the resolution/profile most phones
    // export by default, so ffmpeg re-encodes it (via QProcess, right here on
    // the phone) into a profile the Q5 can actually decode.
    Q_INVOKABLE QString resolve(const QString &mxcUri, const QString &key = QString(), const QString &iv = QString(), const QString &sha256 = QString(), bool isVideo = false);

    // Reads localFilePath, uploads it, then emits uploadFinished.
    Q_INVOKABLE void upload(const QString &localFilePath);

    // Same "return cached path or start async fetch" contract as resolve(),
    // but via the dedicated /thumbnail endpoint at the given pixel size --
    // much cheaper than a full-resolution download for the small avatars
    // shown in the room list and message bubbles. Room/member avatars are
    // never E2EE-encrypted (per spec), so there's no decrypt path here.
    Q_INVOKABLE QString resolveThumbnail(const QString &mxcUri, int width = 96, int height = 96);

    // A fresh "file://" path (a new filename every call, so back-to-back
    // recordings never collide) under the same cache directory downloads
    // use, for bb.multimedia.AudioRecorder's outputUrl.
    Q_INVOKABLE QString newRecordingPath();

    // Returns a "file://" path if already cached; otherwise starts an async
    // fetch (instagramVideoReady() fires later) and returns an empty string.
    // instagramUrl is an Instagram post/Reel page link (see
    // SyncEngine/MessageListModel's extractMediaFields(), which flags these
    // via content.external_url) -- the Beeper Instagram bridge never gives
    // BBport an actual playable video, just a thumbnail and this link.
    // Fetching shells out (via QProcess) to BerryCore's on-device youtube-dl
    // binary, which extracts and downloads the real video (fragile: breaks
    // if Instagram changes their page markup, or if the linked post isn't
    // actually a video/Reel).
    Q_INVOKABLE QString fetchInstagramVideo(const QString &instagramUrl);

    // Returns the cached carousel slides -- [{type: "image"|"video", url:
    // "file://..."}, ...] in carousel order -- if already fetched;
    // otherwise starts an async fetch (instagramCarouselReady() fires
    // later) and returns an empty list. instagramUrl is the same post-page
    // link fetchInstagramVideo() takes, but here yt-dlp is asked to grab
    // the WHOLE carousel (Instagram's multi-image/video "swipe" posts) via
    // its playlist support, one file per slide, instead of a single Reel.
    // A specific slide's own share link carries a carousel_share_child_
    // media_id query parameter identifying which slide was shared -- QML
    // uses that to decide whether to open this single-image viewer or this
    // gallery, but fetching/caching here is always keyed by the base post
    // URL (query string stripped) so every slide of the same carousel
    // shares one cached result regardless of which one was actually shared
    // into the chat -- see conversation.
    Q_INVOKABLE QVariantList fetchInstagramCarousel(const QString &instagramUrl);

    // Transcodes localFilePath (the AudioRecorder's own m4a/AAC output) to
    // Ogg/Opus via ffmpeg, then uploads the result -- but the uploadFinished
    // signal still reports localFilePath itself (not the throwaway .ogg
    // temp file), so callers can key off the exact path they asked to send.
    // Falls back to uploading localFilePath as-is if the transcode fails.
    // Ogg/Opus (rather than the m4a/AAC MediaManager would otherwise upload
    // unchanged) is what lets other Matrix clients render this as an inline
    // voice-message bubble instead of a generic "sent an audio file" link --
    // see MessageListModel::onUploadFinished()'s
    // "org.matrix.msc3245.voice_message" annotation, which is only added
    // when this actually produced audio/ogg.
    Q_INVOKABLE void uploadAudioAsOgg(const QString &localFilePath);

    // Trial alternative to the in-app videoViewerPage (NativeVideoPlayer +
    // mm-renderer): hands localFileUrl to BB10's Invocation Framework with
    // no target specified, so the system picks the default video card (the
    // native Videos app) to play it instead of rendering inside BBport.
    // Kept alongside the in-app player rather than replacing it, so main.qml
    // can be pointed back at navigationPane.openVideo() with a one-line
    // revert if this doesn't work out.
    Q_INVOKABLE void openVideoExternally(const QString &localFileUrl);

    // Appends a timestamped line to
    // /accounts/1000/shared/misc/bbport_debug.log. Exists because there's
    // no way to see qDebug()/console.log() output from a real device
    // without a signed debug token to attach a debugger -- shared/misc
    // (not this app's own private sandbox) so it's a plain `cat
    // bbport_debug.log` away from Term49/any on-device shell, no PC round-
    // trip needed.
    Q_INVOKABLE void debugLog(const QString &line);

signals:
    void mediaReady(const QString &mxcUri, const QString &localFileUrl);
    void mediaFailed(const QString &mxcUri);
    void uploadFinished(const QString &localFilePath, const QString &mxcUri, const QString &mimeType, bool ok);
    void thumbnailReady(const QString &mxcUri, const QString &localFileUrl);
    void thumbnailFailed(const QString &mxcUri);
    void instagramVideoReady(const QString &instagramUrl, const QString &localFileUrl);
    void instagramVideoFailed(const QString &instagramUrl);
    // items: see fetchInstagramCarousel()'s doc comment.
    void instagramCarouselReady(const QString &instagramUrl, const QVariantList &items);
    void instagramCarouselFailed(const QString &instagramUrl);

private slots:
    void onDownloadFinished();
    void onUploadFinished();
    void onThumbnailDownloadFinished();
    void onFfmpegFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void onFfmpegError(QProcess::ProcessError error);
    void onYoutubeDlFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void onYoutubeDlError(QProcess::ProcessError error);
    void onCarouselYoutubeDlFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void onCarouselYoutubeDlError(QProcess::ProcessError error);
    void onCarouselTimeout();
    void onCarouselEncodeFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void onCarouselEncodeError(QProcess::ProcessError error);
    void onCarouselSlideFetchFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void onCarouselSlideFetchError(QProcess::ProcessError error);
    void onAudioTranscodeFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void onAudioTranscodeError(QProcess::ProcessError error);

private:
    struct CryptoInfo {
        QString key;
        QString iv;
        QString sha256;
    };

    QString cachePathFor(const QString &mxcUri) const;
    QString cachePathForThumbnail(const QString &mxcUri, int width, int height) const;
    static bool splitMxc(const QString &mxcUri, QString *server, QString *mediaId);
    QString mimeTypeForFile(const QString &path) const;
    // Decrypts an E2EE media blob per the Matrix "EncryptedFile" format
    // (AES-256-CTR, key.k as unpadded base64url, iv as base64). Returns
    // false (leaving *plaintextOut untouched) on a bad key/iv or a ciphertext
    // sha256 mismatch against info.sha256 (skipped if info.sha256 is empty).
    static bool decryptFile(const QByteArray &ciphertext, const CryptoInfo &info, QByteArray *plaintextOut);
    void finishFfmpegJob(QProcess *proc, bool succeeded);
    void finishYoutubeDlJob(QProcess *proc, bool succeeded);
    void finishAudioTranscodeJob(QProcess *proc, bool succeeded);
    void finishCarouselJob(QProcess *proc, bool succeeded);
    // Video slides yt-dlp downloads are muxed at Instagram's own export
    // resolution/profile (just DASH video+audio combined into one
    // container), which the Q5's hardware decoder can't handle -- same
    // "audio fine, video stays black" limitation as regular video
    // messages (see resolve()'s isVideo branch, whose exact ffmpeg args
    // this reuses). Confirmed on-device: sizing the ForeignWindowControl
    // correctly (matching videoViewerPage) did not fix a black carousel
    // video, pointing at a codec/profile problem rather than a window-
    // binding one.
    void finishCarouselEncodeJob(QProcess *proc, bool succeeded);
    // Starts the video-re-encode fan-out (see finishCarouselEncodeJob's
    // comment) for the given, already-final item list, then finalizes
    // immediately if none of them are video. Shared by finishCarouselJob()
    // (no photo slides were missing) and finishCarouselSlideFetch()'s last
    // caller (photo slides needed the per-slide img_index retry below).
    void startCarouselVideoEncodes(const QString &baseUrl, const QString &instagramUrl, const QVariantList &items);
    // Emits instagramCarouselReady/Failed once every video item's re-encode
    // (if any) has finished -- a no-op (returns immediately) while any are
    // still pending.
    void finalizeCarouselIfDone(const QString &baseUrl);
    // yt-dlp's Instagram extractor tries to resolve "video formats" for
    // every entry of a mixed photo+video carousel and errors out on the
    // photo ones ("No video formats found!") -- confirmed a known,
    // maintainer-closed-wontfix yt-dlp limitation (github.com/yt-dlp/yt-dlp
    // issue #7569), not something fixable via a flag on the single
    // --yes-playlist run finishCarouselJob() already does. Instagram's own
    // "<post url>?img_index=N" (a real, documented, 1-based per-slide link)
    // lets a missing slide be fetched on its own, going through the exact
    // same single-post extraction path that already works correctly for a
    // plain (non-carousel) Instagram photo post. finishCarouselJob() kicks
    // this off for whichever indices its own glob didn't produce, up to
    // the total slide count yt-dlp itself already printed in its stdout.
    void finishCarouselSlideFetch(QProcess *proc, bool succeeded);
    // No-op (returns immediately) while any per-slide fetch is still
    // pending; once all have landed, merges their results into the
    // already-known items and proceeds to startCarouselVideoEncodes().
    void finalizeCarouselSlideFetchesIfDone(const QString &baseUrl);
    // Strips any query string -- see fetchInstagramCarousel()'s doc comment
    // for why the base post URL, not a specific slide's own link, is the
    // actual cache/fetch key.
    static QString carouselBaseUrl(const QString &instagramUrl);
    QString carouselCacheKey(const QString &baseUrl) const;
    QString carouselManifestPath(const QString &baseUrl) const;
    QVariantList loadCarouselManifest(const QString &baseUrl) const;
    void saveCarouselManifest(const QString &baseUrl, const QVariantList &items) const;

    struct FfmpegJob {
        QString mxcUri;
        QString inPath;
        QString outPath;
    };

    struct AudioUploadJob {
        QString originalPath;
        QString outPath;
    };

    struct CarouselVideoEncodeJob {
        QString baseUrl;
        QString inPath;   // yt-dlp's raw muxed output, removed once re-encode succeeds
        QString outPath;  // Q5-compatible re-encoded file
        int itemIndex;    // position within CarouselPending::items to patch
    };

    struct CarouselPending {
        QString instagramUrl;
        QVariantList items;   // "video" entries get their url patched in place as encodes land
        int pendingEncodes;
    };

    struct CarouselSlideFetchJob {
        QString baseUrl;
        QString slotPrefix; // this one fetch's own unique output prefix, globbed afterward for its real filename/extension
    };

    struct CarouselSlideFetchPending {
        QString instagramUrl;
        QVariantList items; // phase-1 (playlist run) items, before any missing slides are merged in
        int pendingFetches;
    };

    MatrixApi *m_api;
    QString m_cacheDir;
    QSet<QString> m_inFlight;
    QHash<QNetworkReply*, QString> m_downloadTarget;
    QHash<QString, CryptoInfo> m_cryptoInfo; // mxcUri -> decryption params, consumed in onDownloadFinished
    QSet<QString> m_videoDownload; // mxcUri, consumed in onDownloadFinished
    QHash<QProcess*, FfmpegJob> m_ffmpegJobs;
    QHash<QNetworkReply*, QString> m_uploadSourcePath;
    QHash<QNetworkReply*, QString> m_uploadMimeType;
    QSet<QString> m_thumbInFlight; // cache paths currently being fetched
    QHash<QNetworkReply*, QString> m_thumbDownloadTarget; // reply -> mxcUri
    QHash<QNetworkReply*, QString> m_thumbCachePath; // reply -> its cache path
    QHash<QProcess*, QString> m_ytdlTarget; // process -> instagramUrl
    QHash<QProcess*, QString> m_ytdlOutTemplate; // process -> youtube-dl -o template (to locate the actual output file)
    QHash<QProcess*, QString> m_carouselTarget; // process -> instagramUrl (the exact slide link that was tapped, for the signal's identity)
    QHash<QProcess*, QString> m_carouselOutPrefix; // process -> output filename prefix (to glob the resulting per-slide files)
    // A carousel fetch expands into a whole yt-dlp playlist run (multiple
    // slides, each its own network fetch) instead of one Reel's single
    // download -- if Instagram serves a login/consent wall or otherwise
    // hangs mid-playlist, yt-dlp can sit with no output and no exit forever.
    // Without this watchdog, that leaves the QML gallery's "Caricamento
    // carosello..." overlay stuck permanently, since neither
    // instagramCarouselReady nor instagramCarouselFailed would ever fire.
    QHash<QProcess*, QTimer*> m_carouselProcTimer; // process -> its watchdog timer (cleared on normal finish)
    QHash<QTimer*, QProcess*> m_carouselTimeoutTarget; // watchdog timer -> the process it guards
    QHash<QProcess*, CarouselVideoEncodeJob> m_carouselEncodeJobs;
    QHash<QString, CarouselPending> m_carouselPending; // baseUrl -> items awaiting any in-flight video re-encodes
    QHash<QProcess*, CarouselSlideFetchJob> m_carouselSlideJobs;
    QHash<QString, CarouselSlideFetchPending> m_carouselSlideFetchPending; // baseUrl -> phase-1 items awaiting any missing-slide (img_index) fetches
    QHash<QProcess*, AudioUploadJob> m_audioUploadJobs;
    QHash<QString, QString> m_uploadPathRemap; // transcoded temp path -> original path, consumed in onUploadFinished
    bb::system::InvokeManager *m_invokeManager;
    int m_recordingCounter;
};

#endif /* MEDIAMANAGER_HPP_ */
