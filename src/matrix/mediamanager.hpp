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
    // Fetching shells out (via QProcess) to parth-dl (a maintained,
    // pip-installed Python library, github.com/parthmax2/parth-dl), which
    // extracts and downloads the real video with no login -- see
    // startInstagramFetch()'s doc comment for why this replaced an earlier
    // yt-dlp-based implementation entirely.
    Q_INVOKABLE QString fetchInstagramVideo(const QString &instagramUrl);

    // Returns the cached carousel slides -- [{type: "image"|"video", url:
    // "file://..."}, ...] in carousel order -- if already fetched;
    // otherwise starts an async fetch (instagramCarouselReady() fires
    // later) and returns an empty list. instagramUrl is the same post-page
    // link fetchInstagramVideo() takes, but here parth-dl is asked for the
    // WHOLE carousel (Instagram's multi-image/video "swipe" posts) -- every
    // slide, image and video alike, in one call. A specific slide's own
    // share link carries a carousel_share_child_media_id query parameter
    // identifying which slide was shared -- QML uses that to decide whether
    // to open this single-image viewer or this gallery, but fetching/
    // caching here is always keyed by the base post URL (query string
    // stripped) so every slide of the same carousel shares one cached
    // result regardless of which one was actually shared into the chat --
    // see conversation.
    Q_INVOKABLE QVariantList fetchInstagramCarousel(const QString &instagramUrl);

    // Transcodes localFilePath (the AudioRecorder's own m4a/AAC output) to
    // Ogg/Opus, then uploads the result -- but the uploadFinished signal
    // still reports localFilePath itself (not the throwaway .ogg temp
    // file), so callers can key off the exact path they asked to send.
    // Falls back to uploading localFilePath as-is if the transcode fails.
    // Two steps, not one ffmpeg call: this BerryCore ffmpeg build has no
    // libopus encoder at all (confirmed via a real device's own
    // `ffmpeg -encoders`), so ffmpeg here only decodes+resamples the AAC to
    // 48kHz PCM (a native decoder/muxer, unaffected by that), and
    // OggOpusEncoder (already-linked libopus, same library
    // OggOpusDecoder already uses for playback) does the actual Opus
    // encode + Ogg muxing in-process -- see finishAudioTranscodeJob().
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
    void onInstagramFetchFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void onInstagramFetchError(QProcess::ProcessError error);
    void onInstagramFetchTimeout();
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
    void finishAudioTranscodeJob(QProcess *proc, bool succeeded);
    // Shared by fetchInstagramVideo() and fetchInstagramCarousel() -- see
    // its own doc comment in the .cpp for why parth-dl replaced an earlier
    // yt-dlp-based implementation entirely (yt-dlp can't get a mixed
    // carousel's photo entries at all; parth-dl's get_info() API returns
    // every entry, video and photos alike, with no login). isCarousel
    // picks which of instagramVideoReady/Failed vs. instagramCarouselReady/
    // Failed finishInstagramFetch() emits.
    void startInstagramFetch(const QString &instagramUrl, const QString &fetchUrl, const QString &outPrefix, bool isCarousel);
    void finishInstagramFetch(QProcess *proc, bool succeeded);
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
        QString outPath; // final .ogg destination -- written by OggOpusEncoder, not ffmpeg (see uploadAudioAsOgg())
        QString wavPath; // ffmpeg's own intermediate output: originalPath's audio, decoded+resampled to 48kHz mono PCM
    };

    struct InstagramFetchJob {
        QString instagramUrl; // the exact link callers/signals identify the request by
        QString outPrefix;    // globbed afterward for every "<outPrefix>_NN.<ext>" file parth-dl produced
        bool isCarousel;      // picks instagramVideoReady/Failed vs. instagramCarouselReady/Failed
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
    QHash<QProcess*, InstagramFetchJob> m_instagramFetchJobs;
    // A carousel fetch can involve several of parth-dl's own network
    // round trips (one per entry) instead of one Reel's single download --
    // if Instagram serves a login/consent wall or otherwise stalls, the
    // Python process can sit with no output and no exit forever. Without
    // this watchdog, that leaves the QML gallery's "Caricamento
    // carosello..." overlay stuck permanently, since neither
    // instagramCarouselReady nor instagramCarouselFailed would ever fire.
    QHash<QProcess*, QTimer*> m_instagramProcTimer; // process -> its watchdog timer (cleared on normal finish)
    QHash<QTimer*, QProcess*> m_instagramTimeoutTarget; // watchdog timer -> the process it guards
    QHash<QProcess*, AudioUploadJob> m_audioUploadJobs;
    QHash<QString, QString> m_uploadPathRemap; // transcoded temp path -> original path, consumed in onUploadFinished
    bb::system::InvokeManager *m_invokeManager;
    int m_recordingCounter;
};

#endif /* MEDIAMANAGER_HPP_ */
