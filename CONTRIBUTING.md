# Contributing to Beport

Thanks for taking an interest in a BlackBerry 10 Matrix client in 2026. This document covers how to get a working development setup, how this codebase is organized, and the conventions/gotchas that will save you time.

## Development environment

You need:

- **BlackBerry Native SDK (BBNDK) 10.3.1** — provides the `blackberry-armv7le-qcc`/`blackberry-x86-qcc` qmake specs, the `arm-blackberry-qnx8eabi-gcc` 4.8.3 cross-toolchain, and the Cascades/Qt 4.8 headers and libraries. Momentics IDE is not required; everything here is buildable from the command line.
- A BB10 device to actually test on. The simulator can build and run the plain Matrix client/messaging/E2EE code, but **not** the native TLS transport (device-only, see below) or anything depending on BerryCore.
- (Optional, for on-device diagnostics) [BerryCore](https://github.com/sw7ft/BerryCore) with `dropbear` or `term49-web` installed, so you have a shell on the device — see "Debugging without Momentics" below.

### Building from the CLI

```sh
export QNX_HOST=/path/to/bbndk/host_10_3_1_12/<platform>/x86
export QNX_TARGET=/path/to/bbndk/target_10_3_1_995/qnx6
export PATH="$QNX_HOST/usr/bin:$PATH"   # needed so make can find sh.exe/qcc/moc on Windows hosts

cd Beport
make -I "$QNX_TARGET/usr/include" Device-Debug
```

The root `Makefile` is a thin wrapper (`include mk/cs-base.mk`, shipped inside the BBNDK target tree) that dispatches to per-configuration subdirectories (`arm/`, `arm-p/`, `x86/`), each with its own qmake-generated Makefile. **Never run `qmake` directly in the project root** — it will overwrite this dispatcher Makefile with a single-configuration one and break every other build target (`Device-Release`, `Simulator-Debug`, ...) until you reconstruct it from a clean BBNDK project template. If you need a quick one-off qmake invocation for experimentation, do it inside `arm/`, `arm-p/`, or `x86/` directly, or in a scratch copy of the tree.

Bump `<buildId>` in `bar-descriptor.xml` for every build you intend to sideload over a previous install — BB10 refuses to "downgrade" to an equal-or-lower version+buildId.

### Project layout

```
src/                    C++ sources
  main.cpp              Entry point
  applicationui.cpp     Wires up all the Matrix backend objects and loads main.qml
  matrix/                Matrix protocol + platform integration layer
    matrixapi.*          Low-level HTTP request building (client-server API)
    tlsnetworkreply.*     Native mbedTLS-backed QNetworkReply (device builds only)
    tlsnetworkaccessmanager.* Routes https:// through the above, http:// through Qt's own
    syncengine.*          /sync long-poll loop, event parsing, room state
    olmcryptomanager.*     Olm/Megolm E2EE, SAS device verification
    keybackupmanager.*     SSSS + Secure Key Backup, Recovery Key handling
    messagelistmodel.*     Per-room message list + sending (text/media/reactions/edits)
    roomlistmodel.*        Room list (inbox) + search
    mediamanager.*          Media download/upload/cache, video transcode, Instagram Reels
    timelinestore.*         Cross-room message cache
    notificationmanager.*   BlackBerry Hub notifications
    nativevideoplayer.*     Direct mm-renderer video playback (bypasses Cascades' MediaPlayer bug)
    oggopusdecoder.*        Voice message Ogg/Opus -> WAV decode
    base58.*                Recovery Key encoding
assets/                 QML UI (main.qml is the whole app's UI; everything else is a supporting component)
third_party/            Vendored libolm, mbedTLS, opus (headers + prebuilt static libs)
tools/                  Standalone helper scripts (see tools/tls-bridge-proxy.py's own docstring — largely superseded by native TLS, kept as a documented fallback)
```

## Code conventions

- **No comments explaining *what* the code does** — names should do that. Comments exist to explain *why*, especially a non-obvious constraint, a workaround for a specific confirmed bug, or something that would otherwise surprise a reader. If you find yourself deleting a comment because the code already says it, that's correct.
- **Diagnose root causes, not symptoms.** Several bugs fixed in this project's history turned out to be one-character argument-order swaps or base64 alphabet mismatches between a vendored header and the actual compiled library — found by reproducing the exact wrong output in an isolated script and comparing byte-for-byte against a known-correct reference, not by guessing.
- Match the existing style in whichever file you're editing (this project does not use a single company-wide style guide; each file is internally consistent).

### Cascades/QML1 gotchas

These cost real debugging time to discover; please don't rediscover them the hard way:

- **`Connections {}` reliability is unverified** in this Cascades build. Use the "property mirror" pattern instead: `property X watcherName: source.property` + `onWatcherNameChanged: { ... }`.
- **A `ListItemComponent` delegate cannot see the document's context properties or resolve sibling `id`s.** Only `ListItemData` — the row's own bound data — is reachable from inside a delegate. If a delegate needs to call into C++, stash a small QObject with the needed methods into the row's own data (see `MessageRowActions` in `messagelistmodel.hpp`) rather than trying to reference a context property by name from the delegate.
- **`Button` has no `background` property** (only `Container` does) — QML throws "Invalid property". Use `ImageButton` (with `defaultImageSource`) for a chrome-less icon button instead.
- **`ActionItem` has no `visible` property.** You cannot conditionally hide one entry in a `contextActions`/`actions` list based on row data; either make the action a safe no-op when it doesn't apply, or build the actions list itself dynamically in JS (untested in this codebase — the no-op approach is what's used today, see `MessageRowActions::edit()`/`copy()`).
- Before assuming a Cascades class has a property you expect, check the real header under `<BBNDK>/target_10_3_1_995/qnx6/usr/include/bb/cascades/` — the two bullets above were both found by guessing wrong first.

### Debugging without Momentics

BlackBerry's debug-token servers are no longer available, so attaching Momentics' live debugger to a real device isn't an option for this project. The established workflow instead:

1. Write diagnostics straight to a file the device shell can read, e.g. `/accounts/1000/shared/misc/beport_debug.log` (shared storage — readable from any app/shell, unlike `QDir::homePath()` which is this app's own private sandbox and needs a PC round-trip via `blackberry-deploy -getFile` to retrieve).
2. Get a shell on the device via [BerryCore](https://github.com/sw7ft/BerryCore)'s `dropbear` (SSH) or `term49-web` (browser-based terminal, easier to type into from a PC keyboard than the device's own touchscreen).
3. `cat`/`tail` the log file from that shell.

`MediaManager::debugLog()` is a ready-made `Q_INVOKABLE` for this from both C++ and QML.

## Reporting bugs / proposing changes

- Open an issue describing the symptom, your BB10 device model, and (if relevant) the relevant excerpt of a device log per the above.
- For pull requests: keep them focused on one change; explain *why* in the PR description, not just what changed.
