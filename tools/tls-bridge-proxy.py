#!/usr/bin/env python3
"""
Bridges BBport (BlackBerry 10, TLS 1.0-only) to a modern TLS-only Matrix
homeserver such as matrix.beeper.com.

Listens on plain HTTP and forwards every request to the target homeserver
over HTTPS using the *host's* up-to-date TLS stack (via Python's own ssl
module -- no extra dependency for this core job). Point BBport's
"Homeserver" field at wherever this ends up running instead of
https://matrix.beeper.com directly.

Runs fine two ways:
  - On your PC: point BBport at http://<PC LAN IP>:8008. Needs a PC on the
    same network, always running this script, whenever you use the app.
  - Directly on a rooted BB10 phone (e.g. via BerryCore's qpkg Python):
    point BBport at http://127.0.0.1:8008 instead, and the app works
    standalone with no PC involved. This entire script -- including the
    megolm-session-import feature below -- is stdlib-only (its AES-256 is a
    small vendored pure-Python implementation, see aes256_ctr_xor(), rather
    than depending on the "cryptography" package, whose Rust/C backend has
    no prebuilt QNX/ARM wheel and can't realistically be built there), so it
    should run on whatever Python 3 BerryCore provides with zero pip
    installs. ffmpeg-based video transcoding (see transcode_video_for_q5())
    is a nice-to-have, not a requirement: NativeVideoPlayer (the app's
    mm-renderer wrapper) plays arbitrary H.264/etc. content directly, so if
    ffmpeg is missing or its build doesn't support the encoder settings used
    here (_transcode_video() below falls back to sending the original video
    untouched either way, logging why), videos still download and play --
    they just aren't re-encoded to a predictable size/bitrate first.

Optionally also decrypts an Element "Export E2E room keys" file once at
startup and serves the resulting Megolm session list locally at
GET /bbport/megolm-sessions, for homeservers (e.g. Beeper) that block the
standard Secure Key Backup API to third-party clients. This never talks to
the homeserver -- it's a pure local file decrypt, using the same
PBKDF2-HMAC-SHA512 + AES-256-CTR + HMAC-SHA256 "megolm session export"
format documented in matrix-org/matrix-spec-proposals#1701.

Usage:
    python tls-bridge-proxy.py [target_host] [listen_port] [export_file] [export_passphrase]

Defaults: target_host=matrix.beeper.com, listen_port=8008
export_file/export_passphrase are optional; omit both to skip key import.

Note: the passphrase is passed on the command line for simplicity (this is
a personal, local-only dev tool) -- it will be visible to other processes
on the same machine via the process list while the proxy runs.
"""
import base64
import json
import os
import re
import ssl
import subprocess
import sys
import tempfile
import http.client
import http.server
import urllib.parse

# A vendored CA bundle (curl.se/ca/cacert.pem, Mozilla's root store), used
# explicitly instead of relying on the host's own system CA store. On a
# rooted BB10 phone via BerryCore, Python's ssl module has no such store to
# fall back to at all -- every HTTPS connection fails with
# "certificate verify failed: unable to get local issuer certificate" -- so
# this makes the bridge work the same way on any host regardless of what
# (if anything) it has configured system-wide.
_CACERT_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), "cacert.pem")
if os.path.exists(_CACERT_PATH):
    # SSL_CERT_FILE is OpenSSL's own standard env var for "use this CA
    # bundle when nothing else is configured" -- setting it here (before
    # anything creates an SSL context) makes every ssl.create_default_context()
    # in this process pick it up automatically, not just our own explicit
    # _SSL_CONTEXT below. That includes yt-dlp (see
    # fetch_instagram_video_bytes()), which makes its own HTTPS requests
    # with no way to hand it our context directly -- without this, it hit
    # the exact same "unable to get local issuer certificate" error.
    os.environ.setdefault("SSL_CERT_FILE", _CACERT_PATH)
_SSL_CONTEXT = ssl.create_default_context(cafile=_CACERT_PATH) if os.path.exists(_CACERT_PATH) else ssl.create_default_context()

TARGET_HOST = sys.argv[1] if len(sys.argv) > 1 else "matrix.beeper.com"
LISTEN_PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 8008
EXPORT_FILE = sys.argv[3] if len(sys.argv) > 3 else None
EXPORT_PASSPHRASE = sys.argv[4] if len(sys.argv) > 4 else None

HOP_BY_HOP = {"host", "connection", "content-length", "transfer-encoding"}

MEGOLM_SESSIONS_CACHE = None  # list[dict] once decrypted, or [] if unavailable


# --- Pure-Python AES-256 (encrypt-only) + CTR mode -------------------------
# Vendored instead of using the "cryptography" package so the megolm export
# decrypt below needs nothing beyond the stdlib -- important on a host like
# a rooted BB10 phone via BerryCore, where "cryptography"'s Rust/C backend
# has no prebuilt QNX/ARM wheel and very likely can't be built there either.
# PBKDF2 and HMAC are already in the stdlib (hashlib.pbkdf2_hmac, hmac); AES
# is the only piece Python's stdlib has no cipher for at all, hence just
# that piece is hand-rolled here. CTR mode only ever needs AES *encryption*
# of the counter (the same keystream XORs both ways), so no decryption key
# schedule/InvSubBytes/InvMixColumns is needed. This file is decrypted once
# at proxy startup, not per-request, so pure-Python performance is fine.
_AES_SBOX = [
    0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
    0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
    0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
    0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
    0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
    0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
    0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
    0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
    0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
    0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
    0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
    0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
    0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
    0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
    0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
    0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16,
]
_AES_RCON = [0x01,0x02,0x04,0x08,0x10,0x20,0x40,0x80,0x1b,0x36,0x6c,0xd8,0xab,0x4d]


def _gf_mul(a, b):
    result = 0
    for _ in range(8):
        if b & 1:
            result ^= a
        hi = a & 0x80
        a = (a << 1) & 0xff
        if hi:
            a ^= 0x1b
        b >>= 1
    return result


def _aes256_key_schedule(key32):
    # AES-256: Nk=8 32-bit words of key, Nr=14 rounds, Nb=4 columns/round key.
    nk, nr, nb = 8, 14, 4
    w = [list(key32[4 * i:4 * i + 4]) for i in range(nk)]
    for i in range(nk, nb * (nr + 1)):
        temp = list(w[i - 1])
        if i % nk == 0:
            temp = temp[1:] + temp[:1]
            temp = [_AES_SBOX[b] for b in temp]
            temp[0] ^= _AES_RCON[i // nk - 1]
        elif i % nk == 4:
            temp = [_AES_SBOX[b] for b in temp]
        w.append([w[i - nk][j] ^ temp[j] for j in range(4)])
    return w


def _aes256_encrypt_block(w, block16):
    nr = 14
    state = [[block16[r + 4 * c] for c in range(4)] for r in range(4)]

    def add_round_key(round_):
        for c in range(4):
            word = w[round_ * 4 + c]
            for r in range(4):
                state[r][c] ^= word[r]

    add_round_key(0)
    for round_ in range(1, nr + 1):
        for r in range(4):
            for c in range(4):
                state[r][c] = _AES_SBOX[state[r][c]]
        for r in range(1, 4):
            state[r] = state[r][r:] + state[r][:r]
        if round_ != nr:
            for c in range(4):
                a0, a1, a2, a3 = state[0][c], state[1][c], state[2][c], state[3][c]
                state[0][c] = _gf_mul(a0, 2) ^ _gf_mul(a1, 3) ^ a2 ^ a3
                state[1][c] = a0 ^ _gf_mul(a1, 2) ^ _gf_mul(a2, 3) ^ a3
                state[2][c] = a0 ^ a1 ^ _gf_mul(a2, 2) ^ _gf_mul(a3, 3)
                state[3][c] = _gf_mul(a0, 3) ^ a1 ^ a2 ^ _gf_mul(a3, 2)
        add_round_key(round_)

    return bytes(state[r][c] for c in range(4) for r in range(4))


def aes256_ctr_xor(key32, iv16, data):
    """NIST SP800-38A CTR mode: the 16-byte iv is the initial counter (as a
    128-bit big-endian integer), incremented by one per 16-byte block. The
    same operation encrypts and decrypts (XOR with the AES-encrypted
    counter keystream), which is exactly what's needed here since this only
    ever decrypts."""
    w = _aes256_key_schedule(key32)
    counter = int.from_bytes(iv16, "big")
    out = bytearray()
    for i in range(0, len(data), 16):
        keystream = _aes256_encrypt_block(w, counter.to_bytes(16, "big"))
        chunk = data[i:i + 16]
        out.extend(b ^ k for b, k in zip(chunk, keystream))
        counter = (counter + 1) % (1 << 128)
    return bytes(out)


def decrypt_megolm_export(file_path, passphrase):
    """Decrypts an Element "Export E2E room keys" file. Returns the list of
    session export objects (dicts with algorithm/room_id/session_id/
    session_key/...). Raises ValueError on a bad passphrase or file.
    """
    import hmac as hmac_mod
    import hashlib

    with open(file_path, "r", encoding="utf-8") as f:
        text = f.read()

    match = re.search(
        r"-----BEGIN MEGOLM SESSION DATA-----(.*?)-----END MEGOLM SESSION DATA-----",
        text, re.DOTALL)
    if not match:
        raise ValueError("File non riconosciuto: mancano i marcatori BEGIN/END MEGOLM SESSION DATA.")

    raw = base64.b64decode("".join(match.group(1).split()))
    if len(raw) < 1 + 16 + 16 + 4 + 32 or raw[0] != 1:
        raise ValueError("File corrotto o versione del formato non supportata.")

    salt = raw[1:17]
    iv = raw[17:33]
    rounds = int.from_bytes(raw[33:37], "big")
    ciphertext = raw[37:-32]
    expected_mac = raw[-32:]

    # PBKDF2 with this many rounds (Element typically exports at 500,000) is
    # deliberately slow to brute-force -- on a PC's OpenSSL-backed hashlib
    # it's near-instant, but on a phone CPU without the same acceleration
    # this can genuinely take a couple of minutes with zero visible progress
    # otherwise, which looks exactly like a hang. Don't Ctrl+C this step.
    print("  deriving encryption key (PBKDF2, %d rounds -- this can take a couple of minutes on a phone CPU, be patient)..." % rounds, flush=True)
    derived = hashlib.pbkdf2_hmac("sha512", passphrase.encode("utf-8"), salt, rounds, dklen=64)
    print("  key derived, verifying...", flush=True)
    aes_key, hmac_key = derived[:32], derived[32:]

    actual_mac = hmac_mod.new(hmac_key, raw[:-32], hashlib.sha256).digest()
    if not hmac_mod.compare_digest(actual_mac, bytes(expected_mac)):
        raise ValueError("Passphrase errata (verifica HMAC fallita).")

    plaintext = aes256_ctr_xor(aes_key, iv, ciphertext)

    sessions = json.loads(plaintext.decode("utf-8"))
    if not isinstance(sessions, list):
        raise ValueError("Formato JSON inatteso nel file decifrato.")
    return sessions


if EXPORT_FILE and EXPORT_PASSPHRASE:
    try:
        MEGOLM_SESSIONS_CACHE = decrypt_megolm_export(EXPORT_FILE, EXPORT_PASSPHRASE)
        print("Chiavi Megolm importate: %d sessioni da %s" % (len(MEGOLM_SESSIONS_CACHE), EXPORT_FILE))
    except Exception as exc:
        print("ATTENZIONE: import chiavi Megolm fallito: %s" % exc)
        MEGOLM_SESSIONS_CACHE = []
elif EXPORT_FILE or EXPORT_PASSPHRASE:
    print("ATTENZIONE: servono sia export_file sia export_passphrase per importare le chiavi; import saltato.")
    MEGOLM_SESSIONS_CACHE = []


def fetch_url_bytes(url, headers=None, max_redirects=6):
    """Plain HTTPS GET with manual redirect following, done host-side (so
    it always uses a modern TLS stack) -- used for reaching Instagram
    itself, a separate concern from the Matrix homeserver bridging _proxy()
    does above."""
    headers = dict(headers or {})
    for _ in range(max_redirects):
        parsed = urllib.parse.urlsplit(url)
        conn = http.client.HTTPSConnection(parsed.hostname, parsed.port or 443, timeout=30, context=_SSL_CONTEXT)
        path = parsed.path + (("?" + parsed.query) if parsed.query else "")
        req_headers = dict(headers)
        req_headers["Host"] = parsed.hostname
        try:
            conn.request("GET", path, headers=req_headers)
            resp = conn.getresponse()
            status = resp.status
            resp_headers = resp.getheaders()
            data = resp.read()
        finally:
            conn.close()
        if status not in (301, 302, 303, 307, 308):
            if status >= 400:
                raise RuntimeError("HTTP %d fetching %s" % (status, url))
            return data
        location = next((v for k, v in resp_headers if k.lower() == "location"), None)
        if not location:
            raise RuntimeError("Redirect senza Location per %s" % url)
        url = urllib.parse.urljoin(url, location)
    raise RuntimeError("Troppi redirect per %s" % url)


def fetch_instagram_video_bytes(post_url):
    """Extracts and downloads the underlying video for a public Instagram
    post/Reel URL (the Beeper Instagram bridge only ever gives BBport a
    thumbnail image plus this post link -- see
    com.beeper.unresolved_media/external_url on the m.image event -- never
    an actual video).

    Delegates the actual extraction to yt-dlp rather than hand-rolled page
    scraping: verified on-device (well, PC-side, but the same code) that
    Instagram no longer exposes a video URL anywhere in the page HTML it
    serves, to a browser OR a link-preview crawler User-Agent, even for a
    confirmed real Reel -- no og:video, no "video_url" JSON key, nothing.
    yt-dlp's Instagram extractor is actively maintained specifically to
    track Instagram's own (undocumented, frequently-changing) internal API,
    which is a losing battle to keep up with by hand here. It's a pure-
    Python package with no mandatory dependencies, so `pip install yt-dlp`
    should work even on a rooted BB10 phone via BerryCore. Imported lazily
    (here, not at module top) so the rest of this proxy still runs fine if
    it isn't installed -- only this one feature needs it.

    Still fragile in the sense that any Instagram post/Reel extractor can
    break when Instagram changes things -- just now someone else's job to
    keep patched (yt-dlp ships frequent releases; `pip install -U yt-dlp`
    if this starts failing across the board).
    """
    try:
        import yt_dlp
    except ImportError:
        raise RuntimeError("yt-dlp non installato su questo host. Installa con: pip install yt-dlp")

    ydl_opts = {"quiet": True, "no_warnings": True, "skip_download": True, "format": "best"}
    try:
        with yt_dlp.YoutubeDL(ydl_opts) as ydl:
            info = ydl.extract_info(post_url, download=False)
    except Exception as exc:
        raise RuntimeError("yt-dlp non e' riuscito a leggere questo post: %s" % exc)

    video_url = info.get("url")
    if not video_url:
        # Some extractions return only a "formats" list rather than a single
        # top-level url -- pick the highest-resolution one that actually has
        # a video track (skips audio-only DASH streams).
        candidates = [f for f in (info.get("formats") or [])
                      if f.get("url") and f.get("vcodec") not in (None, "none")]
        if not candidates:
            raise RuntimeError("Nessun video trovato in questo post (non e' un Reel/video).")
        candidates.sort(key=lambda f: (f.get("height") or 0, f.get("width") or 0))
        video_url = candidates[-1]["url"]

    return fetch_url_bytes(video_url)


def transcode_video_for_q5(input_bytes):
    """Re-encodes a video with ffmpeg for the Q5. The earlier black-screen
    symptom turned out to be a bug in Cascades' own bb::multimedia::
    MediaPlayer/ForeignWindowControl wrapper, confirmed by an on-device A/B
    test against a bare mm-renderer sample -- NOT a codec/profile/resolution
    issue (H.264 High Profile, Baseline, and even ancient H.263/3GP all hit
    the exact same black screen through the buggy wrapper). Now that the app
    talks to mm-renderer directly (NativeVideoPlayer), there is no more
    reason to cripple quality down to feature-phone level. Still transcodes
    (rather than passing the original through untouched) to keep files a
    predictable, moderate size and a broadly-compatible container/profile,
    just at a much higher quality ceiling. Raises RuntimeError (with
    ffmpeg's stderr) on failure; the caller (do_POST) turns that into a 502
    the app treats as a failed download.
    """
    with tempfile.TemporaryDirectory() as tmp:
        in_path = os.path.join(tmp, "in")
        with open(in_path, "wb") as f:
            f.write(input_bytes)

        out_path = os.path.join(tmp, "out.mp4")
        cmd = [
            "ffmpeg", "-y", "-i", in_path,
            "-vf", "scale='min(1280,iw)':-2",
            "-c:v", "libx264", "-profile:v", "high", "-level", "4.0",
            "-preset", "fast",
            "-b:v", "2500k", "-maxrate", "2500k", "-bufsize", "5000k",
            "-c:a", "aac", "-b:a", "128k", "-ar", "44100",
            "-movflags", "+faststart",
            out_path,
        ]
        proc = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        if proc.returncode != 0:
            raise RuntimeError(proc.stderr.decode("utf-8", errors="replace")[-2000:])

        with open(out_path, "rb") as f:
            return f.read()


class ProxyHandler(http.server.BaseHTTPRequestHandler):
    def _transcode_video(self):
        content_length = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(content_length) if content_length > 0 else b""
        print("  transcoding video (%d bytes)..." % len(body), flush=True)
        try:
            out_bytes = transcode_video_for_q5(body)
            print("  transcode done (%d -> %d bytes)" % (len(body), len(out_bytes)), flush=True)
        except Exception as exc:
            # mm-renderer (NativeVideoPlayer) plays arbitrary H.264/etc.
            # content directly now -- transcoding exists to keep file size/
            # bitrate predictable, not for compatibility (confirmed: a real
            # Instagram Reel's original encode played fine with zero
            # transcoding). So a broken ffmpeg here (missing entirely, or --
            # as seen with BerryCore's build -- present but rejecting
            # -preset/-profile:v, suggesting its libx264 isn't a full/
            # standard build) is degraded to "just send the original bytes"
            # rather than failing every single video download outright.
            print("  transcode failed (%s), sending original video untouched: %s" % (type(exc).__name__, exc), flush=True)
            out_bytes = body

        self.send_response(200)
        self.send_header("Content-Type", "video/mp4")
        self.send_header("Content-Length", str(len(out_bytes)))
        self.end_headers()
        self.wfile.write(out_bytes)

    def _instagram_video(self):
        query = urllib.parse.urlsplit(self.path).query
        post_url = (urllib.parse.parse_qs(query).get("url") or [""])[0]
        if not post_url:
            message = b"missing url parameter"
            self.send_response(400)
            self.send_header("Content-Length", str(len(message)))
            self.end_headers()
            self.wfile.write(message)
            return

        print("  fetching instagram video for %s ..." % post_url)
        try:
            video_bytes = fetch_instagram_video_bytes(post_url)
        except Exception as exc:
            print("  instagram fetch failed: %s" % exc)
            message = str(exc).encode("utf-8", errors="replace")
            self.send_response(502)
            self.send_header("Content-Length", str(len(message)))
            self.end_headers()
            self.wfile.write(message)
            return

        # Reuse the same Q5-friendly re-encode as normal chat videos when
        # ffmpeg is available. Instagram's own encode is usually already a
        # broadly-compatible H.264/AAC mp4, so this is a nice-to-have here
        # (consistent size/profile), not a hard requirement -- fall back to
        # the original bytes rather than failing the whole request if it's
        # missing or errors out.
        try:
            video_bytes = transcode_video_for_q5(video_bytes)
        except Exception as exc:
            print("  instagram video transcode skipped: %s" % exc)

        print("  instagram video ready (%d bytes)" % len(video_bytes))
        self.send_response(200)
        self.send_header("Content-Type", "video/mp4")
        self.send_header("Content-Length", str(len(video_bytes)))
        self.end_headers()
        self.wfile.write(video_bytes)

    def _serve_megolm_sessions(self):
        body = json.dumps(MEGOLM_SESSIONS_CACHE if MEGOLM_SESSIONS_CACHE is not None else []).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _proxy(self, method):
        content_length = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(content_length) if content_length > 0 else None

        headers = {k: v for k, v in self.headers.items() if k.lower() not in HOP_BY_HOP}
        host = TARGET_HOST
        path = self.path
        headers["Host"] = host

        # Beeper's media endpoints 307-redirect (often to a different host
        # entirely) for the actual bytes. Qt4 (what BB10 ships) never
        # auto-follows redirects -- QNetworkRequest::FollowRedirectsAttribute
        # doesn't exist before Qt 5.6 -- and even if it did, the redirect
        # target may itself require modern TLS that this proxy exists
        # specifically to route around. So redirects are followed HERE, on
        # the PC, and only the final response is ever sent back to BBport.
        status, resp_headers, resp_body = None, [], b""
        for _ in range(6):
            conn = http.client.HTTPSConnection(host, timeout=60, context=_SSL_CONTEXT)
            print("  >> %s https://%s%s headers=%r" % (method, host, path, headers))
            try:
                conn.request(method, path, body=body, headers=headers)
                resp = conn.getresponse()
                status = resp.status
                resp_headers = resp.getheaders()
                resp_body = resp.read()
            except Exception as exc:
                # Printed here (not just sent back as the 502 body) since
                # BBport itself only ever shows a generic "network error" to
                # the user -- this is the only place the actual cause (DNS
                # failure, TLS/certificate error, connection refused, ...)
                # is visible.
                print("  !! errore connessione a %s: %s: %s" % (host, type(exc).__name__, exc), flush=True)
                message = ("Errore proxy verso %s: %s" % (host, exc)).encode()
                self.send_response(502)
                self.send_header("Content-Length", str(len(message)))
                self.end_headers()
                self.wfile.write(message)
                return
            finally:
                conn.close()

            if status not in (301, 302, 303, 307, 308):
                if status >= 400:
                    print("  -> %d from %s%s: %s" % (status, host, path, resp_body[:300]))
                break
            location = next((v for k, v in resp_headers if k.lower() == "location"), None)
            if not location:
                break
            print("  -> %d redirect from %s%s to %s" % (status, host, path, location))
            parsed = urllib.parse.urlsplit(urllib.parse.urljoin("https://%s%s" % (host, path), location))
            new_host = parsed.hostname
            path = parsed.path + (("?" + parsed.query) if parsed.query else "")
            if new_host != host:
                # Cross-host hop (e.g. Beeper's homeserver redirecting media
                # downloads to a presigned S3/R2 URL): don't forward the
                # original request's Matrix Authorization/etc. headers to an
                # unrelated third-party host -- a presigned URL's signature
                # only covers whatever's listed in its own X-Amz-SignedHeaders
                # (observed here as just "host"), so nothing else is needed or
                # wanted. (Adding x-amz-content-sha256 was tried first, since
                # an early, differently-broken version of this forwarding got
                # "400 Missing x-amz-content-sha256" from R2 -- but a plain
                # unauthenticated GET straight from a browser needs no such
                # header, and adding it here only ever produced "403
                # SignatureDoesNotMatch" from R2, regardless of its value.)
                headers = {}
            host = new_host
            headers["Host"] = host
            if status == 303:
                method, body = "GET", None
                if "Content-Length" in headers:
                    del headers["Content-Length"]

        self.send_response(status)
        for key, value in resp_headers:
            if key.lower() in HOP_BY_HOP or key.lower() == "location":
                continue
            self.send_header(key, value)
        self.send_header("Content-Length", str(len(resp_body)))
        self.end_headers()
        self.wfile.write(resp_body)

    def do_GET(self):
        if self.path == "/bbport/megolm-sessions":
            self._serve_megolm_sessions()
            return
        if self.path.startswith("/bbport/instagram-video"):
            self._instagram_video()
            return
        self._proxy("GET")

    def do_POST(self):
        if self.path == "/bbport/transcode-video":
            self._transcode_video()
            return
        self._proxy("POST")

    def do_PUT(self):
        self._proxy("PUT")

    def do_DELETE(self):
        self._proxy("DELETE")

    def log_message(self, fmt, *args):
        print("%s %s" % (self.address_string(), fmt % args))


if __name__ == "__main__":
    server = http.server.ThreadingHTTPServer(("0.0.0.0", LISTEN_PORT), ProxyHandler)
    print("BBport TLS bridge: http://0.0.0.0:%d -> https://%s" % (LISTEN_PORT, TARGET_HOST))
    print("Premi Ctrl+C per fermarlo.")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
