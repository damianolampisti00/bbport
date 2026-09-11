# Beport

A native [Matrix](https://matrix.org) chat client for BlackBerry 10, written in C++ against the Cascades UI framework (Qt 4.8 / BB10 Native SDK 10.3.1).

Beport talks directly to any Matrix homeserver — including [Beeper](https://www.beeper.com), which bridges iMessage, WhatsApp, Instagram DMs, Signal, and more into Matrix rooms — with full end-to-end encryption, native TLS 1.2 (working around BB10's decade-old system OpenSSL, which only speaks TLS 1.0 and is rejected outright by modern homeservers), and on-device media handling (photos, videos, voice messages, and even Instagram Reels).

This is a hobby project built for a specific piece of ten-year-old hardware most people don't own. If you're one of the people who still carries a BlackBerry 10 device daily, welcome — this is for you.

## Screenshots

*(add screenshots here — e.g. `docs/screenshots/inbox.png`, `docs/screenshots/chat.png`)*

## Features

- **Messaging**: text, photos, videos, voice messages (recorded on-device, Opus/Ogg), files, replies, emoji reactions, message editing, message deletion, typing indicators, read receipts.
- **End-to-end encryption**: full Olm (1:1) and Megolm (group) session support, SAS ("emoji") device verification, Secure Key Backup (SSSS) unlock via Recovery Key entered right on the login screen — decrypts your entire message history the moment you log in, no separate manual step.
- **Rooms**: search, unread counts, avatars (including the Matrix convention of showing the other participant's own profile picture for direct messages that never set a room avatar), a "hidden chats" list for rooms you don't want cluttering the inbox.
- **Media**: downloads and uploads work over Beport's own native TLS 1.2 stack — no external proxy required. Video is transcoded on-device with `ffmpeg` for the phone's hardware decoder, and Instagram Reels shared into a chat (via Beeper's Instagram bridge) can be fetched and played in-app via `yt-dlp`, both running locally through [BerryCore](https://github.com/sw7ft/BerryCore), a community QNX/BB10 userland.
- **Native video playback**: talks to QNX's `mm-renderer` service directly, bypassing a real bug in Cascades' own `bb::multimedia::MediaPlayer` wrapper that renders solid black video on real hardware.
- **Notifications**: BlackBerry Hub integration, including an explicit opt-in into Instant Preview (the pop-up banner style other Hub-integrated apps use), and disabled entirely during the very first sync so logging in doesn't produce a notification for every message in your history.
- **Fast resume**: the sync position and a snapshot of your room list are cached to disk, so relaunching the app (or recovering from a crash) picks up where it left off with an incremental sync instead of re-downloading everything.

## Known limitations

This is a beta. Specifically:

- **No persistent login.** Credentials are not saved; you re-enter them every time the app is (re)started. (The fast-resume sync cache above still applies once you're logged back in.)
- **No in-app room creation or invites.** You can join and accept invites to existing rooms, but can't start a brand new conversation or invite someone from within Beport. Create the room elsewhere (e.g. Element, or the Beeper apps for bridged chats) and it will show up here.
- **No background/headless push.** Notifications only fire while the app process is alive (foreground or backgrounded, not fully terminated by the OS) — there's no separate headless service keeping the sync loop running after you close the app.
- **Edits aren't shown "in place."** Sending an edit produces a real Matrix `m.replace` event (other clients like Element render it correctly, collapsed into the original message), but Beport itself currently displays it as a new message prefixed with `*` rather than replacing the original bubble.
- **Single account only.**

## Requirements

- A BlackBerry 10 device. Developed against a BlackBerry Q5; other BB10 hardware should work but is untested.
- [BerryCore](https://github.com/sw7ft/BerryCore) installed on the device, with the `ffmpeg`, `yt-dlp` (via `pip install yt-dlp` under BerryCore's own Python 3), and `python3` packages available — required for video transcoding and Instagram Reel playback. Everything else (login, messaging, E2EE, photo/video/voice download) works without BerryCore.
- To build from source: [BlackBerry Native SDK (BBNDK) 10.3.1](https://developer.blackberry.com) with the `blackberry-armv7le-qcc` toolchain (gcc 4.8.3).

## Building

Beport builds entirely from the command line — no BlackBerry Momentics IDE required (useful since Momentics' on-device debugging needs a signed debug token, which BlackBerry's token servers no longer issue).

```sh
# From the BBNDK host tools, with QNX_HOST/QNX_TARGET set to your install:
export QNX_HOST=/path/to/bbndk/host_10_3_1_12/<platform>/x86
export QNX_TARGET=/path/to/bbndk/target_10_3_1_995/qnx6
export PATH="$QNX_HOST/usr/bin:$PATH"

cd Beport
make -I "$QNX_TARGET/usr/include" Device-Debug     # or Device-Release / Device-Profile
```

This produces `arm/o.le-v7-g/Beport` (Debug) or `arm/o.le-v7/Beport.so` (Release), matching the paths declared in `bar-descriptor.xml`.

To package a `.bar` for sideloading, use BlackBerry's `blackberry-nativepackager` against `bar-descriptor.xml`, or a third-party packaging/sideload tool (e.g. Darcy BB Tools) if you don't have a code-signing debug token.

Bump `<buildId>` in `bar-descriptor.xml` before each new package you intend to install over a previous one — BB10 requires the version+buildId tuple to strictly increase.

### Simulator builds

The `blackberry-x86-qcc` simulator target is supported by the project (`Beport.pro` has a `simulator{}` scope), but native TLS (`BEPORT_HAVE_NATIVE_TLS`) is device-only; the simulator falls back to Qt's own `QSslSocket`-backed networking, which cannot reach a modern homeserver directly (see [Architecture](#architecture) below) — it would need `tools/tls-bridge-proxy.py` pointed at as the homeserver instead.

## Installing

Sideload the built `.bar` with any BB10 sideloading tool. No debug token is required for a normal install — only for attaching Momentics' live debugger, which this project's whole diagnostic workflow (see `CONTRIBUTING.md`) is built to avoid needing.

## Usage

See [`docs/USAGE.md`](docs/USAGE.md) for a walkthrough of login (including unlocking encrypted history with your Recovery Key), messaging, media, and the long-press message menu.

## Architecture

A few of the non-obvious technical decisions, for anyone digging into the code:

- **Native TLS 1.2 via mbedTLS, not OpenSSL.** BB10's system OpenSSL only speaks TLS 1.0, which every modern Matrix homeserver rejects outright. Statically linking a modern OpenSSL 3.x was tried first and works cryptographically, but its relocation table (~20,000 entries, from pulling in nearly all of libcrypto) overwhelms BB10's 2014-era dynamic loader and crashes on launch. mbedTLS 2.28 LTS produces a binary an order of magnitude smaller with a few hundred relocations, and just works. See `src/matrix/tlsnetworkreply.cpp`/`tlsnetworkaccessmanager.cpp` — a `QNetworkAccessManager`/`QNetworkReply` subclass pair that slots the mbedTLS transport in underneath Qt's existing high-level networking API, so the rest of the app talks to it exactly like it would talk to Qt's own networking.
- **On-device media processing via BerryCore, not a PC proxy.** An earlier iteration of this project needed a companion Python script running on a PC to bridge TLS and to transcode video/extract Instagram Reels. All of that has since moved on-device: BerryCore (a community-maintained QNX/BB10 userland, distinct from BlackBerry's own SDK) ships modern `ffmpeg` and Python 3, and `yt-dlp` runs on that Python to pull real video out of Instagram Reel/post links (which Beeper's Instagram bridge otherwise only ever hands over as a static thumbnail). Both are invoked directly via `QProcess` from `src/matrix/mediamanager.cpp`.
- **End-to-end encryption** is implemented directly against [libolm](https://gitlab.matrix.org/matrix-org/olm) 3.2.16's C API — no bindings library. `src/matrix/olmcryptomanager.cpp` handles Olm/Megolm session lifecycle and SAS verification; `src/matrix/keybackupmanager.cpp` handles Secure Secret Storage (SSSS) and Secure Key Backup, including recovering historical message keys from the server-side backup using the user's Recovery Key.
- **Cascades/QML1 quirks worth knowing before touching `assets/main.qml`:**
  - `Connections {}` element reliability is unverified in this Cascades build; a "property mirror" pattern (`property X watcher: source.property; onWatcherChanged: {...}`) is used everywhere instead.
  - A `ListItemComponent` delegate cannot see the document's context properties or resolve sibling `id`s — only `ListItemData` (the row's own bound data) is reachable. Anything a delegate needs to trigger in C++ is reached through a small per-row action object stashed in the row's own data (see `MessageRowActions` in `messagelistmodel.hpp`), not by referencing a context property by name.
  - `Button` has no `background` property (only `Container` does); `ActionItem` has no `visible` property. Both were confirmed against the BBNDK headers after guessing wrong once.

## Third-party components

Beport itself is licensed under the GNU General Public License v3.0 (see [`LICENSE`](LICENSE)). It vendors and links against:

| Component | Used for | License |
|---|---|---|
| [libolm](https://gitlab.matrix.org/matrix-org/olm) 3.2.16 | Olm/Megolm end-to-end encryption | Apache License 2.0 |
| [mbedTLS](https://github.com/Mbed-TLS/mbedtls) 2.28 LTS | Native TLS 1.2 transport | Apache License 2.0 |
| [libopus](https://opus-codec.org/) | Voice message encode/decode | BSD 3-Clause |
| [Twemoji](https://github.com/jdecked/twemoji) v17.0.3 (`assets/emoji/`) | Emoji graphics | CC-BY 4.0, © Twitter, Inc. and other contributors |
| [BerryCore](https://github.com/sw7ft/BerryCore) | On-device `ffmpeg`/`yt-dlp`/Python for media features | Not bundled — a separate userland the user installs themselves |
| BlackBerry Native SDK / Cascades headers | UI framework, platform APIs | Apache License 2.0, © BlackBerry Limited |

No source from any of the above is redistributed in a way that would require relicensing this repository under their terms; Beport's own code is GPLv3 as noted above.

## Contributing

See [`CONTRIBUTING.md`](CONTRIBUTING.md).
