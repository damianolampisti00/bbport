/*
 * Copyright (c) 2011-2015 BlackBerry Limited.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

import bb.cascades 1.4
import bb.cascades.pickers 1.0
import bb.multimedia 1.0
import it.bbport 1.0
import "emojimap.js" as EmojiMap

// Real Matrix-backed client: login, live room list (/sync), conversation
// with text + photo messages, typing indicators and read receipts.
//
// Login <-> chat list is a plain visibility toggle bound to
// matrixApi.loggedIn (a Q_PROPERTY with NOTIFY), not an imperative push/pop,
// so it needs no signal-handler wiring onto an external C++ object.
NavigationPane {
    id: navigationPane
    backButtonsVisible: false
    property bool verifyBannerDismissed: false

    // Video cleanup used to live only in the close "X" button's onClicked,
    // which meant swiping back (instead of tapping it) left mm-renderer
    // still playing in the background. popTransitionEnded fires for every
    // pop regardless of how it happened (swipe or programmatic), so this
    // covers both -- videoViewerPage exposes stopVideo() precisely so this
    // outer scope can call it without needing to know about nativeVideoPlayer
    // directly. Checked via a plain function-existence check since imageViewerPage
    // (which pops through here too) has no such function.
    onPopTransitionEnded: {
        if (page && page.stopVideo) {
            page.stopVideo();
            page.destroy();
        }
    }

    function openConversation(roomId, name) {
        messageListModel.roomId = roomId;
        var page = conversationPage.createObject();
        page.contactName = name;
        navigationPane.push(page);
    }

    // Tapping a notification re-invokes the app with a roomId payload,
    // which ApplicationUI's InvokeManager handler hands to
    // roomListModel.requestOpenRoom() -- see notificationmanager.cpp for
    // where each Notification's InvokeRequest is attached. Pops back to the
    // inbox first rather than pushing on top of whatever's already open, so
    // tapping a notification always lands on exactly that one conversation
    // (not stacked behind/on top of an unrelated page).
    property string pendingRoomWatcher: roomListModel.pendingOpenRoomId
    onPendingRoomWatcherChanged: {
        if (!pendingRoomWatcher) return;
        // No explicit destroy() here on the popped pages -- onPopTransitionEnded
        // above already destroys video pages once their pop animation actually
        // finishes; destroying them again here (synchronously, before that
        // fires) would double-destroy the same object. Plain pages (chat,
        // image viewer) are left undestroyed the same way an ordinary
        // swipe-back already leaves them -- a pre-existing tradeoff, not a
        // new one introduced here.
        while (navigationPane.count() > 1) {
            navigationPane.pop();
        }
        var idx = roomListModel.indexOfRoom(pendingRoomWatcher);
        var name = idx >= 0 ? roomListModel.roomAt(idx).name : "";
        openConversation(pendingRoomWatcher, name);
    }

    // Unlocks the Megolm key backup right away on login (if a Recovery Key
    // was entered on the login screen), before the initial /sync even
    // starts -- so historical encrypted messages have a chance to decrypt
    // as they arrive instead of showing the "encrypted" placeholder first
    // and only getting patched in later via a separate manual unlock.
    property bool loggedInWatcher: matrixApi.loggedIn
    onLoggedInWatcherChanged: {
        if (loggedInWatcher && recoveryKeyField.text.length > 0) {
            keyBackupManager.unlock(recoveryKeyField.text);
        }
    }

    function openImage(localUrl) {
        var page = imageViewerPage.createObject();
        page.imageUrl = localUrl;
        navigationPane.push(page);
    }

    // firstSlideUrl is the one slide already available locally (the exact
    // image/video that was actually shared into the chat) -- shown
    // immediately so the gallery never opens to a blank/loading screen,
    // while MediaManager::fetchInstagramCarousel() (kicked off by the
    // caller, not here) fills in the rest of the carousel in the
    // background. See conversation for why carousel-slide shares need this
    // separate gallery instead of the plain single-image viewer.
    function openInstagramCarousel(instagramUrl, firstSlideType, firstSlideUrl) {
        var page = carouselViewerPage.createObject();
        page.instagramUrl = instagramUrl;
        page.firstSlideType = firstSlideType;
        page.firstSlideUrl = firstSlideUrl;
        navigationPane.push(page);
    }

    function openVideo(localUrl, durationMs, videoWidth, videoHeight) {
        var page = videoViewerPage.createObject();
        page.durationMs = durationMs > 0 ? durationMs : 0;
        page.videoWidth = videoWidth > 0 ? videoWidth : 0;
        page.videoHeight = videoHeight > 0 ? videoHeight : 0;
        page.videoUrl = localUrl;
        navigationPane.push(page);
    }

    attachedObjects: [
        ComponentDefinition {
            id: videoViewerPage
            Page {
                property string videoUrl: ""
                property bool playing: false
                property int durationMs: 0
                property int videoWidth: 0
                property int videoHeight: 0
                // openVideo() sets page.videoUrl AFTER createObject() already
                // returned -- by which point Component.onCreationCompleted has
                // already fired (it runs synchronously during createObject(),
                // before the caller's next line executes), so wiring the
                // player there always saw videoUrl still at its default "".
                // A property-change handler fires at the right time instead:
                // once when videoUrl is actually assigned.
                function tryStartPlayback() {
                    if (videoUrl.length === 0) return;
                    // Talks to mm-renderer directly (NativeVideoPlayer, see
                    // src/matrix/nativevideoplayer.cpp) rather than
                    // bb::multimedia::MediaPlayer, which bound correctly
                    // (boundToWindow/windowAttached/videoDimensions all
                    // fired) but always rendered solid black on-device --
                    // confirmed a Cascades wrapper bug, not a codec/platform
                    // one, via an on-device A/B test against a bare
                    // mm-renderer sample. Still targets this same
                    // ForeignWindowControl via the same
                    // "screen:?wingrp=...&winid=..." URL scheme that sample
                    // used.
                    playing = nativeVideoPlayer.play(videoUrl, "bbportVideoSurface", fwcVideoSurface.windowGroup, fwcVideoSurface.pixelWidth, fwcVideoSurface.pixelHeight);
                }
                onVideoUrlChanged: tryStartPlayback()
                // Called from navigationPane's onPopTransitionEnded (outer
                // scope, reachable regardless of swipe-back vs a
                // programmatic pop) so mm-renderer always actually stops
                // instead of continuing to play behind the popped page.
                function stopVideo() {
                    nativeVideoPlayer.stop();
                    playing = false;
                }
                Container {
                    layout: DockLayout {}
                    background: Color.Black
                    // Video needs a dedicated Screen Graphics window surface --
                    // it can't render directly into a Cascades ImageView/Label
                    // like a static image can. ForeignWindowControl "punches a
                    // hole" in the scene for that surface; mm-renderer (via
                    // NativeVideoPlayer) creates a Screen child window that
                    // joins this control's windowId/windowGroup.
                    ForeignWindowControl {
                        id: fwcVideoSurface
                        // Sized to letterbox: mm-renderer scales the video
                        // to fill whatever rectangle this control occupies,
                        // cropping instead of adding bars if the aspect
                        // ratio doesn't match -- a fixed 720x720 square made
                        // every non-square video look "zoomed in" (cropped).
                        // Fitting the control itself to the video's own
                        // aspect ratio (known from the message's
                        // content.info.w/h, same field the reply/thumbnail
                        // logic already uses) within a 720x720 box gives
                        // real letterboxing: the surrounding black
                        // background (this Container) shows as the bars.
                        property int maxBox: 720
                        // Raw physical-pixel values, kept separate from
                        // preferredWidth/Height below: Cascades layout
                        // properties are DU by default, and a bare number
                        // (no ui.px() wrapping) is interpreted as DU, not
                        // pixels -- confirmed against BlackBerry's own
                        // helloforeignwindow sample, which always wraps a
                        // ForeignWindowControl's size in ui.px() for exactly
                        // this reason. tryStartPlayback() passes these two
                        // (not preferredWidth/Height, which after ui.px()
                        // wrapping below no longer hold a pixel count) to
                        // NativeVideoPlayer.play() as its destWidth/
                        // destHeight, which mm-renderer needs in real pixels
                        // to know its own destination rectangle.
                        // Declared with a plain default, then bound as a
                        // separate statement below -- a `{...}` block only
                        // parses as a property's *value* when assigned to an
                        // already-declared property (like preferredWidth
                        // used to be bound directly), not as a `property int
                        // x: {...}` declaration's own initializer.
                        property int pixelWidth: maxBox
                        property int pixelHeight: maxBox
                        pixelWidth: {
                            if (videoWidth <= 0 || videoHeight <= 0) return maxBox;
                            var scale = Math.min(maxBox / videoWidth, maxBox / videoHeight);
                            return Math.round(videoWidth * scale);
                        }
                        pixelHeight: {
                            if (videoWidth <= 0 || videoHeight <= 0) return maxBox;
                            var scale = Math.min(maxBox / videoWidth, maxBox / videoHeight);
                            return Math.round(videoHeight * scale);
                        }
                        preferredWidth: ui.px(pixelWidth)
                        preferredHeight: ui.px(pixelHeight)
                        horizontalAlignment: HorizontalAlignment.Center
                        verticalAlignment: VerticalAlignment.Center
                        windowId: "bbportVideoSurface"
                        // No WindowProperty.SourceSize here -- that forces
                        // the window's "sampled region" to match this
                        // control's own size instead of the video's native
                        // resolution, which is why an earlier attempt
                        // showed the video at native size pinned to the
                        // top-left instead of scaled to fill.
                        updatedProperties: WindowProperty.Position | WindowProperty.Size
                        visible: boundToWindow
                    }
                    Container {
                        horizontalAlignment: HorizontalAlignment.Center
                        verticalAlignment: VerticalAlignment.Bottom
                        bottomMargin: ui.du(3)
                        layout: StackLayout {}
                        Container {
                            // Scrub bar: NativeVideoPlayer doesn't report
                            // live position (that needs mm-renderer's PPS
                            // event stream, deliberately not wired up here
                            // to avoid re-opening the Cascades/BPS event-loop
                            // conflict this whole player exists to sidestep)
                            // -- position is approximated locally by a timer
                            // while playing, reset on seek. Duration comes
                            // from the message's own content.info.duration
                            // (already known before playback even starts),
                            // same field the audio player uses.
                            visible: durationMs > 0
                            layout: StackLayout { orientation: LayoutOrientation.LeftToRight }
                            leftMargin: ui.du(2); rightMargin: ui.du(2); bottomMargin: ui.du(1)
                            Slider {
                                layoutProperties: StackLayoutProperties { spaceQuota: 1 }
                                fromValue: 0
                                toValue: durationMs > 0 ? durationMs : 1
                                value: nativeVideoPlayer.positionMs
                                onValueChanged: {
                                    if (Math.abs(value - nativeVideoPlayer.positionMs) > 800) {
                                        nativeVideoPlayer.seek(value);
                                    }
                                }
                            }
                            Label {
                                text: {
                                    var posSec = Math.floor(nativeVideoPlayer.positionMs / 1000);
                                    var durSec = Math.floor(durationMs / 1000);
                                    var posMm = Math.floor(posSec / 60);
                                    var posSs = posSec % 60;
                                    var durMm = Math.floor(durSec / 60);
                                    var durSs = durSec % 60;
                                    var posStr = posMm + ":" + (posSs < 10 ? "0" : "") + posSs;
                                    var durStr = durMm + ":" + (durSs < 10 ? "0" : "") + durSs;
                                    return posStr + " / " + durStr;
                                }
                                verticalAlignment: VerticalAlignment.Center
                                leftMargin: ui.du(1)
                                textStyle.base: SystemDefaults.TextStyles.SmallText
                                textStyle.color: Color.White
                            }
                        }
                        Button {
                            text: playing ? "Pause" : "Play"
                            appearance: ControlAppearance.Plain
                            color: Color.create("#2f5eff")
                            preferredWidth: ui.du(24)
                            onClicked: {
                                if (playing) {
                                    nativeVideoPlayer.pause();
                                    playing = false;
                                } else {
                                    nativeVideoPlayer.resume();
                                    playing = true;
                                }
                            }
                        }
                    }
                    attachedObjects: [
                        NativeVideoPlayer {
                            id: nativeVideoPlayer
                        }
                    ]
                }
            }
        },
        ComponentDefinition {
            id: manageHiddenChatsPage
            Page {
                titleBar: TitleBar { title: "Hidden chats" }
                onCreationCompleted: {
                    hiddenChatsDataModel.append(roomListModel.allRoomsForManagement());
                }
                Container {
                    layout: DockLayout {}
                    background: Color.create("#101316")
                    ListView {
                        id: hiddenChatsListView
                        dataModel: ArrayDataModel { id: hiddenChatsDataModel }
                        // Same row-tap-via-onTriggered pattern used
                        // everywhere else in this app (roomView, messageView)
                        // rather than a nested Button inside the delegate --
                        // interactive elements nested inside a
                        // ListItemComponent have repeatedly proven unreliable
                        // in this Cascades build. A tap toggles hidden state.
                        onTriggered: {
                            var item = dataModel.data(indexPath);
                            if (!item) return;
                            if (item.hidden) {
                                roomListModel.unhideRoom(item.roomId);
                            } else {
                                roomListModel.hideRoom(item.roomId);
                            }
                            item.hidden = !item.hidden;
                            dataModel.replace(indexPath[0], item);
                        }
                        listItemComponents: [
                            ListItemComponent {
                                type: ""
                                Container {
                                    layout: StackLayout { orientation: LayoutOrientation.LeftToRight }
                                    leftPadding: ui.du(1.5); rightPadding: ui.du(1.5)
                                    topPadding: ui.du(1); bottomPadding: ui.du(1)
                                    background: Color.create("#1a2026")
                                    Label {
                                        text: ListItemData.name
                                        layoutProperties: StackLayoutProperties { spaceQuota: 1 }
                                        textStyle.color: Color.White
                                        verticalAlignment: VerticalAlignment.Center
                                    }
                                    Label {
                                        text: ListItemData.hidden ? "Hidden -- tap to show" : "Visible -- tap to hide"
                                        textStyle.base: SystemDefaults.TextStyles.SmallText
                                        textStyle.color: Color.create("#9ab8da")
                                        verticalAlignment: VerticalAlignment.Center
                                    }
                                }
                            }
                        ]
                    }
                    Button {
                        text: "‹"
                        appearance: ControlAppearance.Plain
                        preferredWidth: ui.du(7)
                        horizontalAlignment: HorizontalAlignment.Left
                        verticalAlignment: VerticalAlignment.Top
                        topMargin: ui.du(1); leftMargin: ui.du(1)
                        onClicked: navigationPane.pop()
                    }
                }
            }
        },
        ComponentDefinition {
            id: imageViewerPage
            Page {
                property string imageUrl: ""
                Container {
                    layout: DockLayout {}
                    background: Color.Black
                    ScrollView {
                        horizontalAlignment: HorizontalAlignment.Fill
                        verticalAlignment: VerticalAlignment.Fill
                        scrollViewProperties {
                            pinchToZoomEnabled: true
                            minContentScale: 1.0
                            maxContentScale: 4.0
                            scrollMode: ScrollMode.Both
                            initialScalingMethod: ScalingMethod.AspectFit
                        }
                        Container {
                            ImageView {
                                imageSource: imageUrl
                            }
                        }
                    }
                }
            }
        },
        ComponentDefinition {
            id: carouselViewerPage
            Page {
                property string instagramUrl: ""
                // The one slide already available locally (see
                // openInstagramCarousel()'s doc comment) -- shown as the
                // gallery's first item immediately, before the background
                // fetch below can possibly resolve.
                property string firstSlideType: "image"
                property string firstSlideUrl: ""
                property bool loadingMore: true
                property bool loadError: false

                // One slide full-screen at a time (index into
                // carouselDataModel), not a scrollable list -- Cascades has
                // no page-snapping scroll container (confirmed: no
                // SwipeHandler/PanHandler exists in this SDK, and
                // ListView/ScrollView have no snap-to-item mode), so a
                // horizontal ListView would leave slides stopped mid-drag
                // instead of landing exactly on one. Advancing currentIndex
                // ourselves off a raw touch gesture (below) gives an exact,
                // predictable "always shows exactly one whole slide" result.
                property int currentIndex: 0
                // Plain properties explicitly assigned by refreshCurrentSlide()
                // below, rather than binding ImageView/the position label
                // directly to carouselDataModel.value(currentIndex)/.size():
                // a QML binding only re-evaluates when a property it reads
                // changes, and ArrayDataModel's own append()/clear() aren't
                // property changes -- so a binding on .value()/.size() would
                // silently keep showing stale content the moment the
                // placeholder slide gets replaced by the real fetched list
                // (currentIndex staying at 0 both times is exactly that
                // case). Same "explicit assignment over relying on Cascades
                // binding reactivity" lesson as every other property-mirror
                // in this file.
                property string currentSlideType: "image"
                property string currentSlideUrl: ""
                property int slideCount: 0
                property string activeVideoUrl: ""
                property bool videoPlaying: false
                // Touch-down x (Container-local), reset to -1 between
                // gestures; isDown()/isUp()/localX are real Q_PROPERTYs on
                // bb::cascades::TouchEvent (verified against the SDK
                // headers), not a guess -- this is a plain "flick" detector,
                // not a live drag-follow animation, to keep it simple and
                // robust: TouchType.Down records the start x, TouchType.Up
                // compares against it once, no per-frame tracking needed.
                property real swipeStartX: -1

                function refreshCurrentSlide() {
                    slideCount = carouselDataModel.size();
                    activeVideoUrl = "";
                    videoPlaying = false;
                    if (currentIndex < 0 || currentIndex >= slideCount) {
                        currentSlideType = "image";
                        currentSlideUrl = "";
                        return;
                    }
                    var item = carouselDataModel.value(currentIndex);
                    currentSlideType = item ? item.type : "image";
                    currentSlideUrl = item ? item.url : "";
                    if (currentSlideType === "video") activeVideoUrl = currentSlideUrl;
                }
                onActiveVideoUrlChanged: {
                    if (activeVideoUrl.length === 0) return;
                    // Autoplay on arrival, same NativeVideoPlayer/mm-renderer
                    // approach as videoViewerPage (see its own comment for
                    // why: bb::multimedia::MediaPlayer rendered solid black
                    // on-device despite binding correctly).
                    videoPlaying = carouselVideoPlayer.play(activeVideoUrl, "bbportCarouselVideoSurface", fwcCarouselVideoSurface.windowGroup, fwcCarouselVideoSurface.maxBox, fwcCarouselVideoSurface.maxBox);
                }
                // Called from navigationPane's onPopTransitionEnded (see its
                // own comment) -- without this mm-renderer keeps playing
                // behind the popped page, same reasoning as videoViewerPage.
                function stopVideo() {
                    carouselVideoPlayer.stop();
                    videoPlaying = false;
                }

                // dx <= 0 advances forward (left-drag reveals the next
                // slide, matching Instagram's own gallery -- this is the
                // content the carousel came from); dx > 0 goes back. Past
                // either end there's nothing left to reveal, so the same
                // gesture instead exits the viewer -- symmetric with the
                // system's own edge-swipe-back, so it feels like "falling
                // off" the gallery in either direction.
                function handleSwipe(dx) {
                    var threshold = ui.du(15);
                    if (dx <= -threshold) {
                        if (currentIndex < slideCount - 1) {
                            currentIndex = currentIndex + 1;
                            refreshCurrentSlide();
                        } else {
                            navigationPane.pop();
                        }
                    } else if (dx >= threshold) {
                        if (currentIndex > 0) {
                            currentIndex = currentIndex - 1;
                            refreshCurrentSlide();
                        } else {
                            navigationPane.pop();
                        }
                    } else if (activeVideoUrl.length > 0) {
                        // Too short to be a swipe -- a plain tap on a video
                        // slide toggles play/pause instead.
                        if (videoPlaying) {
                            carouselVideoPlayer.pause();
                        } else {
                            carouselVideoPlayer.resume();
                        }
                        videoPlaying = !videoPlaying;
                    }
                }

                // NOT started from onCreationCompleted: openInstagramCarousel()
                // calls createObject() then assigns page.instagramUrl/
                // firstSlideType/firstSlideUrl on the lines AFTER -- but
                // Component.onCreationCompleted fires synchronously *during*
                // createObject(), before any of those assignments happen (the
                // exact same trap videoViewerPage's own onVideoUrlChanged
                // comment already documents for videoUrl). Starting here
                // meant instagramUrl was always still "" the moment this ran,
                // so fetchInstagramCarousel("") hit its own first-line empty
                // check and returned instantly with no fetch, no log, and no
                // way to ever leave the loading state -- exactly the
                // "schermo nero, caricamento a oltranza" symptom, confirmed
                // by there being zero yt-dlp log output at all.
                // firstSlideUrl is the LAST of the three properties the
                // caller assigns, so reacting to its change (not
                // instagramUrl's, set first) guarantees all three are
                // already correct by the time this runs.
                property bool startedLoading: false
                onFirstSlideUrlChanged: {
                    if (startedLoading) return;
                    startedLoading = true;
                    carouselDataModel.append([{"type": firstSlideType, "url": firstSlideUrl}]);
                    refreshCurrentSlide();
                    var cached = mediaManager.fetchInstagramCarousel(instagramUrl);
                    if (cached && cached.length > 0) {
                        loadingMore = false;
                        carouselDataModel.clear();
                        carouselDataModel.append(cached);
                        currentIndex = 0;
                        refreshCurrentSlide();
                    }
                }

                // Same "property mirror" pattern as conversationPage's
                // reelWatcher: a freshly created page picks up whatever
                // result is already sitting on messageListModel from a
                // previous carousel fetch (this or a different post), so
                // the instagramUrl match below (not just loadingMore) is
                // what keeps a stale result from a DIFFERENT post from
                // overwriting this page's slides.
                property variant carouselWatcher: messageListModel.instagramCarouselResult
                onCarouselWatcherChanged: {
                    if (!loadingMore) return;
                    if (carouselWatcher.instagramUrl !== instagramUrl) return;
                    loadingMore = false;
                    if (carouselWatcher.ok && carouselWatcher.items && carouselWatcher.items.length > 0) {
                        carouselDataModel.clear();
                        carouselDataModel.append(carouselWatcher.items);
                        currentIndex = 0;
                        refreshCurrentSlide();
                    } else {
                        loadError = true;
                    }
                }

                Container {
                    layout: DockLayout {}
                    background: Color.Black
                    horizontalAlignment: HorizontalAlignment.Fill
                    verticalAlignment: VerticalAlignment.Fill
                    onTouch: {
                        if (event.isDown()) {
                            swipeStartX = event.localX;
                        } else if (event.isUp() && swipeStartX >= 0) {
                            var dx = event.localX - swipeStartX;
                            swipeStartX = -1;
                            handleSwipe(dx);
                        }
                    }

                    ImageView {
                        visible: currentSlideType !== "video"
                        imageSource: currentSlideUrl
                        scalingMethod: ScalingMethod.AspectFit
                        horizontalAlignment: HorizontalAlignment.Center
                        verticalAlignment: VerticalAlignment.Center
                    }

                    // Full-bleed rather than videoViewerPage's letterboxed
                    // maxBox: that page knows the real content.info width/
                    // height from the message event, this one doesn't (yt-dlp
                    // only reports back {type, url} per slide -- see
                    // MediaManager::fetchInstagramCarousel()), and a
                    // full-screen gallery reads better full-bleed anyway.
                    ForeignWindowControl {
                        id: fwcCarouselVideoSurface
                        visible: activeVideoUrl.length > 0 && boundToWindow
                        // Fixed pixel size, NOT Fill: this is exactly the
                        // bug videoViewerPage's own ForeignWindowControl
                        // comment warns about -- mm-renderer renders solid
                        // black if the surface it binds to hasn't already
                        // resolved to real pixel dimensions at bind time,
                        // which Fill-based layout sizing doesn't guarantee
                        // has happened yet. videoViewerPage sidesteps this
                        // with a fixed maxBox square (matching its own
                        // "unknown dimensions" fallback, since carousel
                        // items carry no width/height metadata to size
                        // against either) -- same fix here, verbatim.
                        // preferredWidth/Height wrapped in ui.px() (raw
                        // pixels, not DU -- see videoViewerPage's own
                        // fwcVideoSurface comment) so maxBox stays readable
                        // as-is for NativeVideoPlayer.play()'s destWidth/
                        // destHeight, which mm-renderer needs in real pixels.
                        property int maxBox: 720
                        preferredWidth: ui.px(maxBox)
                        preferredHeight: ui.px(maxBox)
                        horizontalAlignment: HorizontalAlignment.Center
                        verticalAlignment: VerticalAlignment.Center
                        windowId: "bbportCarouselVideoSurface"
                        updatedProperties: WindowProperty.Position | WindowProperty.Size
                    }
                    Label {
                        visible: activeVideoUrl.length > 0 && !videoPlaying
                        text: "▶"
                        horizontalAlignment: HorizontalAlignment.Center
                        verticalAlignment: VerticalAlignment.Center
                        textStyle.color: Color.White
                        textStyle.fontSize: FontSize.XLarge
                    }

                    // Position indicator, purely informational (not a tap
                    // target -- swiping/the edge buttons are the only way to
                    // move, same as the rest of this page).
                    Label {
                        visible: slideCount > 1
                        text: (currentIndex + 1) + " / " + slideCount
                        horizontalAlignment: HorizontalAlignment.Center
                        verticalAlignment: VerticalAlignment.Top
                        topMargin: ui.du(1.5)
                        textStyle.color: Color.White
                        textStyle.base: SystemDefaults.TextStyles.SmallText
                    }

                    // Fallback navigation for whenever the swipe gesture
                    // above doesn't land reliably on real hardware -- see
                    // conversation. Plain Buttons (not nested delegate
                    // content) at Page scope are already proven to work
                    // fine here, same as videoViewerPage's Play/Pause.
                    Button {
                        text: "‹"
                        visible: slideCount > 1
                        appearance: ControlAppearance.Plain
                        color: Color.White
                        preferredWidth: ui.du(8)
                        horizontalAlignment: HorizontalAlignment.Left
                        verticalAlignment: VerticalAlignment.Center
                        onClicked: {
                            if (currentIndex > 0) {
                                currentIndex = currentIndex - 1;
                                refreshCurrentSlide();
                            } else {
                                navigationPane.pop();
                            }
                        }
                    }
                    Button {
                        text: "›"
                        visible: slideCount > 1
                        appearance: ControlAppearance.Plain
                        color: Color.White
                        preferredWidth: ui.du(8)
                        horizontalAlignment: HorizontalAlignment.Right
                        verticalAlignment: VerticalAlignment.Center
                        onClicked: {
                            if (currentIndex < slideCount - 1) {
                                currentIndex = currentIndex + 1;
                                refreshCurrentSlide();
                            } else {
                                navigationPane.pop();
                            }
                        }
                    }

                    Container {
                        visible: loadingMore
                        horizontalAlignment: HorizontalAlignment.Center
                        verticalAlignment: VerticalAlignment.Bottom
                        bottomMargin: ui.du(3)
                        layout: StackLayout { orientation: LayoutOrientation.LeftToRight }
                        background: Color.create("#cc1a2026")
                        leftPadding: ui.du(1.5); rightPadding: ui.du(1.5)
                        topPadding: ui.du(0.8); bottomPadding: ui.du(0.8)
                        ActivityIndicator {
                            running: loadingMore
                            preferredWidth: ui.du(3); preferredHeight: ui.du(3)
                            rightMargin: ui.du(1)
                        }
                        Label {
                            text: "Caricamento carosello..."
                            textStyle.color: Color.White
                            verticalAlignment: VerticalAlignment.Center
                        }
                    }

                    Label {
                        visible: loadError
                        text: "Impossibile caricare le altre foto/video di questo post."
                        horizontalAlignment: HorizontalAlignment.Center
                        verticalAlignment: VerticalAlignment.Bottom
                        bottomMargin: ui.du(3)
                        textStyle.color: Color.create("#ff6b6b")
                        multiline: true
                    }

                    attachedObjects: [
                        ArrayDataModel { id: carouselDataModel },
                        NativeVideoPlayer {
                            id: carouselVideoPlayer
                        }
                    ]
                }
            }
        },
        ComponentDefinition {
            id: conversationPage
            Page {
                property string contactName: ""
                property bool isRecording: false
                property string recordingPath: ""
                property bool attachMenuVisible: false
                // sendFailed() previously had no QML listener at all -- a
                // failed send (network error, encryption error, server
                // rejection) was completely silent, since the composer
                // clears optimistically regardless of outcome. Mirrors
                // messageListModel.lastSendError (property mirror trick,
                // same reasoning as seekWatcher below) to show it here.
                property string sendErrorWatcher: messageListModel.lastSendError
                property bool sendErrorVisible: false
                onSendErrorWatcherChanged: {
                    sendErrorVisible = sendErrorWatcher.length > 0;
                }
                // Mirrors messageListModel.editTarget (property mirror trick,
                // same reasoning as sendErrorWatcher above): long-press
                // "Edit" on a message row calls setEditTarget() in C++,
                // which this page-scope watcher picks up to actually fill
                // the composer -- the delegate itself can't reach `composer`
                // by id (same isolated-context limitation as everywhere else
                // in this file).
                property variant editTargetWatcher: messageListModel.editTarget
                onEditTargetWatcherChanged: {
                    if (editTargetWatcher.eventId) {
                        composer.text = editTargetWatcher.body;
                    }
                }
                // Mirrors messageListModel.seekRequestMs so its change can be
                // reacted to at Page scope (same trick as boundRoomId below):
                // a plain property-change handler always works, whereas
                // whether a "Connections {}" element is available in this
                // Cascades/QML1 build without an extra import is unverified.
                property int seekWatcher: messageListModel.seekRequestMs
                onSeekWatcherChanged: {
                    chatAudioPlayer.seekTime(seekWatcher);
                }
                // Fires once a Reel video fetch started from onTriggered above
                // finishes (see MessageListModel::instagramVideoResult) --
                // covers the "wasn't cached yet, had to fetch" case; the
                // already-cached case is handled synchronously in onTriggered
                // itself.
                //
                // instagramVideoResult is a single shared property on
                // messageListModel, not scoped to this page -- a freshly
                // created conversationPage's reelWatcher binding picks up
                // whatever result is *already sitting there* from a
                // previous reel (in this room or another one) the instant
                // the page is created, and QML fires onReelWatcherChanged
                // for that initial value too. Without the reelLoading guard
                // below, that stale value would silently reopen a video
                // page every single time any conversation is opened --
                // stacking up NativeVideoPlayer/mm-renderer windows (each
                // openVideo() creates its own Screen surface that Cascades
                // doesn't automatically hide behind whatever's on top) and
                // making an old reel reappear over a totally unrelated chat.
                // reelLoading is page-local and only ever set true by this
                // exact page's own onTriggered below, so it's a reliable
                // "this result is actually mine" check.
                property variant reelWatcher: messageListModel.instagramVideoResult
                property bool reelLoading: false
                property bool reelError: false
                onReelWatcherChanged: {
                    if (!reelLoading) return;
                    reelLoading = false;
                    if (reelWatcher.ok) {
                        reelError = false;
                        // Trial: hands off to BB10's own Videos app instead of
                        // the in-app videoViewerPage -- see
                        // MediaManager::openVideoExternally(). Revert by
                        // swapping this back to
                        // navigationPane.openVideo(reelWatcher.localFileUrl, 0, 1080, 1920)
                        // if it doesn't work out.
                        mediaManager.openVideoExternally(reelWatcher.localFileUrl);
                    } else {
                        reelError = true;
                    }
                }
                titleBar: TitleBar { title: contactName }
                Container {
                    layout: DockLayout {}
                    background: Color.create("#101316")

                    Container {
                        layout: StackLayout {}
                        Label {
                            text: "Loading earlier messages..."
                            visible: messageListModel.loadingHistory
                            horizontalAlignment: HorizontalAlignment.Center
                            topMargin: ui.du(0.5)
                            textStyle.base: SystemDefaults.TextStyles.SmallText
                            textStyle.color: Color.create("#9ab8da")
                        }
                        Label {
                            text: messageListModel.typingText
                            visible: messageListModel.typingText.length > 0
                            leftPadding: ui.du(1.5); topPadding: ui.du(0.5)
                            textStyle.base: SystemDefaults.TextStyles.SmallText
                            textStyle.color: Color.create("#9ab8da")
                        }
                        ListView {
                            id: messageView
                            dataModel: messageListModel.model
                            layoutProperties: StackLayoutProperties { spaceQuota: 1 }
                            bottomPadding: ui.du(10)
                            leftPadding: ui.du(1); rightPadding: ui.du(1); topPadding: ui.du(1)

                            // messageListModel.model is a fixed QObject* (Q_PROPERTY CONSTANT),
                            // so ListView never sees it "change" -- track roomId directly on this
                            // item instead, since a plain on<Signal> handler only works for
                            // signals/properties declared on the item itself, not on an externally
                            // injected QObject like messageListModel.
                            property string boundRoomId: messageListModel.roomId
                            onBoundRoomIdChanged: {
                                scrollToPosition(ScrollPosition.End, ScrollAnimation.None);
                            }

                            // Auto-loads older messages when the user scrolls up to the
                            // top of the currently-loaded window, instead of a manual
                            // "load more" button. loadOlderMessages() is itself a no-op
                            // while a page is already in flight or history is exhausted,
                            // so re-firing atBeginningChanged while pinned at the top
                            // (e.g. right after a page is prepended) is harmless.
                            attachedObjects: [
                                ListScrollStateHandler {
                                    id: historyScrollHandler
                                    onAtBeginningChanged: {
                                        if (atBeginning) messageListModel.loadOlderMessages();
                                    }
                                }
                            ]

                            // Row taps: routed through the list's own triggered() signal
                            // rather than a touch/gesture handler on a child inside the
                            // delegate, which didn't fire reliably here. Runs in the Page's
                            // scope, so chatAudioPlayer/navigationPane (both siblings on
                            // the Page, not context properties) are safely reachable.
                            onTriggered: {
                                var item = dataModel.data(indexPath);
                                if (!item) return;
                                // instagramUrl is set for BOTH Reels and regular posts/
                                // carousel-slide shares (see extractMediaFields() in
                                // syncengine.cpp/messagelistmodel.cpp) but only a Reel
                                // (.../reel/...) is ever bridged as a thumbnail-only
                                // placeholder with no real video -- a post or carousel
                                // slide (.../p/...) already carries its real image/video
                                // as normal E2EE media, mediaLocalUrl included, exactly
                                // like any other message (confirmed against real bridge
                                // payloads -- see conversation). Checking instagramUrl
                                // before mediaLocalUrl used to route every Instagram
                                // share -- posts included -- through the yt-dlp scrape
                                // flow below, so a plain shared photo could never be
                                // opened fullscreen like a normal image.
                                var isReel = item.instagramUrl && item.instagramUrl.length > 0
                                        && item.instagramUrl.indexOf("/reel/") >= 0;
                                // Instagram's own share action puts this query
                                // parameter on the link only when the user shared one
                                // specific slide from WITHIN a carousel they were
                                // viewing (confirmed against a real carousel-slide
                                // share payload -- see conversation); a plain single-
                                // image/video post never has it. That's the signal for
                                // "open the swipeable gallery" instead of just this one
                                // image, since the attached media here is only ever
                                // that one slide -- the rest of the carousel has to be
                                // fetched separately (see openInstagramCarousel()).
                                var isCarouselSlide = item.instagramUrl && item.instagramUrl.length > 0
                                        && item.instagramUrl.indexOf("carousel_share_child_media_id") >= 0;
                                if (item.msgtype === "m.image" && item.mediaLocalUrl && item.mediaLocalUrl.length > 0 && isCarouselSlide && !isReel) {
                                    navigationPane.openInstagramCarousel(item.instagramUrl, "image", item.mediaLocalUrl);
                                } else if (item.msgtype === "m.image" && item.mediaLocalUrl && item.mediaLocalUrl.length > 0 && !isReel) {
                                    navigationPane.openImage(item.mediaLocalUrl);
                                } else if (item.msgtype === "m.video" && item.mediaLocalUrl && item.mediaLocalUrl.length > 0 && !isReel) {
                                    // Trial: see the reelWatcher branch above. Revert with
                                    // navigationPane.openVideo(item.mediaLocalUrl,
                                    // item.mediaDuration, item.mediaWidth, item.mediaHeight).
                                    mediaManager.openVideoExternally(item.mediaLocalUrl);
                                } else if (item.instagramUrl && item.instagramUrl.length > 0) {
                                    // A genuine Reel (real video never delivered), or the
                                    // rare case of a post/carousel share whose real media
                                    // never resolved locally -- fall back to scraping a
                                    // watchable video off the Instagram page itself.
                                    // fetchInstagramVideo() returns a "file://" path
                                    // immediately if already cached from a previous tap;
                                    // otherwise it starts the fetch and
                                    // messageListModel.instagramVideoResult (watched
                                    // below) fires once it's ready.
                                    reelError = false;
                                    var cached = mediaManager.fetchInstagramVideo(item.instagramUrl);
                                    if (cached && cached.length > 0) {
                                        // Already fetched by an earlier tap (this or a
                                        // previous session) -- instagramVideoResult won't
                                        // fire again for a cache hit, so the probed
                                        // duration/dimensions are read directly here
                                        // instead, the same way onReelWatcherChanged reads
                                        // them off instagramVideoResult for a fresh fetch.
                                        // Trial: see the reelWatcher branch above.
                                        mediaManager.openVideoExternally(cached);
                                    } else {
                                        reelLoading = true;
                                    }
                                } else if (item.msgtype === "m.audio" && item.mediaLocalUrl && item.mediaLocalUrl.length > 0) {
                                    if (item.eventId === messageListModel.playingAudioEventId && messageListModel.audioIsPlaying) {
                                        chatAudioPlayer.pause();
                                    } else {
                                        messageListModel.playingAudioEventId = item.eventId;
                                        chatAudioPlayer.sourceUrl = item.mediaLocalUrl;
                                        chatAudioPlayer.play();
                                    }
                                }
                                // A plain m.text message with no media/reel action has no
                                // single-tap behavior at all -- replying is long-press-only
                                // now (see the "Reply" contextAction below).
                            }

                            // Three approaches to per-row bubble alignment were tried and all
                            // failed on-device: (1) a single delegate whose bubble's own
                            // horizontalAlignment was bound to ListItemData.isOutgoing; (2)
                            // two ListItemComponent types picked via a ListView-level
                            // itemType() function (confirmed via a temporary debug label that
                            // ListItemData.isOutgoing itself was correct per row, so itemType()
                            // just never dispatched on it); (3) two full MessageBubbleContent
                            // copies as DockLayout siblings, one hardcoded Right + visible:
                            // ListItemData.isOutgoing, one hardcoded Left + visible:
                            // !ListItemData.isOutgoing -- still always rendered left, which
                            // means overriding a built-in property (horizontalAlignment, or
                            // even visible) at the instantiation site of a custom component
                            // from another file isn't reliable here either.
                            //
                            // What IS proven reliable on-device (same screenshot: the sender
                            // avatar/name Container correctly appears only on incoming rows)
                            // is a plain "visible" binding on an INLINE Container declared
                            // directly in this same delegate. So this drops horizontalAlignment
                            // (and any property override on MessageBubbleContent) entirely: a
                            // single un-touched MessageBubbleContent sits in a left-to-right
                            // StackLayout between two plain spacer Containers, and only one
                            // spacer's "visible" (hence its spaceQuota participation -- Cascades
                            // fully excludes an invisible node from layout) is ever true,
                            // pushing the bubble to whichever side is actually empty.
                            listItemComponents: [
                                ListItemComponent {
                                    type: ""
                                    Container {
                                        horizontalAlignment: HorizontalAlignment.Fill
                                        topMargin: ui.du(0.8)
                                        layout: StackLayout { orientation: LayoutOrientation.LeftToRight }
                                        // Long-press "Reply", take 3. The first two attempts
                                        // both had the ActionItem's onTriggered try to reach OUT
                                        // by name (messageListModel directly, then a same-document
                                        // id bridge) -- both confirmed dead, since a delegate's
                                        // isolated context can't resolve either identifier.
                                        // This one never needs to: ListItemData.actions is a
                                        // MessageRowActions* (see messagelistmodel.hpp) that
                                        // MessageListModel already stashed into this row's own
                                        // data, so it's reached purely through ListItemData --
                                        // the one thing this scope has always been able to see --
                                        // and the actual call into MessageListModel happens
                                        // entirely in C++ (that object holds a real pointer to
                                        // it), with no QML/JS scope lookup involved at all.
                                        contextActions: [
                                            ActionSet {
                                                title: "Actions"
                                                ActionItem {
                                                    title: "Reply"
                                                    onTriggered: ListItemData.actions.reply()
                                                }
                                                ActionItem {
                                                    title: EmojiMap.emojiAsset("❤️").length > 0 ? "" : "❤️ Like"
                                                    imageSource: EmojiMap.emojiAsset("❤️")
                                                    onTriggered: ListItemData.actions.like()
                                                }
                                                ActionItem {
                                                    title: "Edit"
                                                    onTriggered: ListItemData.actions.edit()
                                                }
                                                ActionItem {
                                                    title: "Copy"
                                                    onTriggered: ListItemData.actions.copy()
                                                }
                                                ActionItem {
                                                    title: "Delete"
                                                    onTriggered: ListItemData.actions.remove()
                                                }
                                            }
                                        ]
                                        Container {
                                            visible: ListItemData.isOutgoing
                                            layoutProperties: StackLayoutProperties { spaceQuota: 1 }
                                        }
                                        MessageBubbleContent {}
                                        Container {
                                            visible: !ListItemData.isOutgoing
                                            layoutProperties: StackLayoutProperties { spaceQuota: 1 }
                                        }
                                    }
                                }
                            ]
                        }
                    }

                    Container {
                        verticalAlignment: VerticalAlignment.Bottom
                        layout: StackLayout { orientation: LayoutOrientation.TopToBottom }

                        Container {
                            // Stays up until dismissed or the next send attempt
                            // (success or failure both re-fire sendErrorWatcher's
                            // onChanged -- a resent success clears it via the
                            // empty-string case in that handler... actually
                            // sendFailed only ever carries non-empty text, so
                            // this only ever shows/updates on an actual failure;
                            // the "✕" is the only way to dismiss a shown one).
                            visible: sendErrorVisible
                            layout: StackLayout { orientation: LayoutOrientation.LeftToRight }
                            leftPadding: ui.du(1); rightPadding: ui.du(1)
                            topPadding: ui.du(0.6); bottomPadding: ui.du(0.6)
                            background: Color.create("#4a2020")
                            Label {
                                text: sendErrorWatcher
                                multiline: true
                                layoutProperties: StackLayoutProperties { spaceQuota: 1 }
                                textStyle.base: SystemDefaults.TextStyles.SmallText
                                textStyle.color: Color.White
                            }
                            Button {
                                text: EmojiMap.emojiAsset("❌").length > 0 ? "" : "❌"
                                imageSource: EmojiMap.emojiAsset("❌")
                                appearance: ControlAppearance.Plain
                                preferredWidth: ui.du(7)
                                onClicked: sendErrorVisible = false
                            }
                        }
                        Container {
                            id: replyBanner
                            // Reply target staged by tapping a message with no other
                            // tap action (see messageView.onTriggered above), or via
                            // long-press "Reply". Cleared either by the "✕" here or
                            // automatically once sendText() consumes it. Styled like a
                            // quote block (colored left bar + sender name + preview)
                            // rather than plain text, so it reads as "this is what
                            // you're replying to" at a glance -- the same visual
                            // language as the reply-preview already shown above
                            // received replies in MessageBubbleContent.qml.
                            //
                            // visible popping straight from false to true (instant
                            // layout inclusion, per Cascades excluding hidden nodes
                            // entirely) read as an abrupt/mechanical appearance -- a
                            // quick fade-in softens it without touching layout timing,
                            // which isn't something this Cascades build lets QML
                            // animate directly.
                            visible: messageListModel.hasReplyTarget
                            attachedObjects: [
                                FadeTransition {
                                    id: replyFadeIn
                                    fromOpacity: 0.0
                                    toOpacity: 1.0
                                    duration: 180
                                }
                            ]
                            onVisibleChanged: {
                                if (visible) replyFadeIn.play();
                            }
                            layout: StackLayout { orientation: LayoutOrientation.LeftToRight }
                            leftPadding: ui.du(1); rightPadding: ui.du(1)
                            topPadding: ui.du(0.6); bottomPadding: ui.du(0.6)
                            background: Color.create("#1a2026")
                            Container {
                                preferredWidth: ui.du(0.5)
                                background: Color.create("#2f5eff")
                                rightMargin: ui.du(0.8)
                            }
                            Container {
                                layoutProperties: StackLayoutProperties { spaceQuota: 1 }
                                Label {
                                    text: "Reply to " + messageListModel.replyTarget.senderShort
                                    textStyle.base: SystemDefaults.TextStyles.SmallText
                                    textStyle.fontWeight: FontWeight.Bold
                                    textStyle.color: Color.create("#2f5eff")
                                }
                                Label {
                                    text: messageListModel.replyTarget.bodyPreview
                                    multiline: false
                                    textStyle.base: SystemDefaults.TextStyles.SmallText
                                    textStyle.color: Color.create("#c4ccd4")
                                }
                            }
                            ImageButton {
                                defaultImageSource: EmojiMap.emojiAsset("❌")
                                preferredWidth: ui.du(7)
                                onClicked: messageListModel.clearReplyTarget()
                            }
                        }
                        Container {
                            id: editBanner
                            // Same shape/behavior as replyBanner above, for long-press
                            // "Edit" -- see editTargetWatcher at Page (NavigationPane)
                            // scope for how composer.text actually gets filled: a
                            // delegate's contextAction can reach ListItemData.actions
                            // (hence MessageListModel, in C++) but not this Container's
                            // sibling `composer` by id, so setEditTarget() just stages the
                            // data and this watcher (which CAN see composer) fills it in.
                            visible: messageListModel.hasEditTarget
                            attachedObjects: [
                                FadeTransition {
                                    id: editFadeIn
                                    fromOpacity: 0.0
                                    toOpacity: 1.0
                                    duration: 180
                                }
                            ]
                            onVisibleChanged: {
                                if (visible) editFadeIn.play();
                            }
                            layout: StackLayout { orientation: LayoutOrientation.LeftToRight }
                            leftPadding: ui.du(1); rightPadding: ui.du(1)
                            topPadding: ui.du(0.6); bottomPadding: ui.du(0.6)
                            background: Color.create("#1a2026")
                            Container {
                                preferredWidth: ui.du(0.5)
                                background: Color.create("#e0a72f")
                                rightMargin: ui.du(0.8)
                            }
                            Label {
                                text: "Editing message"
                                layoutProperties: StackLayoutProperties { spaceQuota: 1 }
                                textStyle.base: SystemDefaults.TextStyles.SmallText
                                textStyle.fontWeight: FontWeight.Bold
                                textStyle.color: Color.create("#e0a72f")
                            }
                            ImageButton {
                                defaultImageSource: EmojiMap.emojiAsset("❌")
                                preferredWidth: ui.du(7)
                                onClicked: {
                                    messageListModel.clearEditTarget();
                                    composer.text = "";
                                }
                            }
                        }
                        Container {
                            layout: StackLayout { orientation: LayoutOrientation.LeftToRight }
                            leftPadding: ui.du(1); rightPadding: ui.du(1)
                            topPadding: ui.du(1); bottomPadding: ui.du(1)
                            background: Color.create("#1a2026")

                            ImageButton {
                                defaultImageSource: EmojiMap.emojiAsset("📎")
                                preferredWidth: ui.du(7)
                                onClicked: attachMenuVisible = true
                            }
                            TextField {
                                id: composer
                                hintText: "Write a message"
                                layoutProperties: StackLayoutProperties { spaceQuota: 1 }
                                onTextChanging: {
                                    messageListModel.setTypingActive(composer.text.length > 0);
                                }
                            }
                            Container {
                                preferredWidth: ui.du(11)
                            preferredHeight: ui.du(8)
                            verticalAlignment: VerticalAlignment.Center
                            layout: DockLayout {}
                            enabled: !messageListModel.sending
                            background: isRecording ? Color.create("#c0392b") : Color.Transparent
                            EmojiIcon {
                                emoji: composer.text.length > 0 ? "➡" : (isRecording ? "⏹" : "🎤")
                                iconSize: ui.du(4)
                                horizontalAlignment: HorizontalAlignment.Center
                                verticalAlignment: VerticalAlignment.Center
                            }
                            onTouch: {
                                if (!event.isUp()) return;
                                if (composer.text.length > 0) {
                                    messageListModel.sendText(composer.text);
                                    composer.text = "";
                                    messageListModel.setTypingActive(false);
                                } else if (isRecording) {
                                    var recordedMs = audioRecorder.duration;
                                    audioRecorder.reset();
                                    isRecording = false;
                                    if (recordedMs > 500) {
                                        messageListModel.sendAudio(recordingPath, recordedMs);
                                    }
                                } else {
                                    recordingPath = mediaManager.newRecordingPath();
                                    audioRecorder.outputUrl = "file://" + recordingPath;
                                    audioRecorder.record();
                                    isRecording = true;
                                }
                            }
                        }
                        }
                    }

                    // Declared last among this DockLayout's children so they
                    // paint on top of messageView -- Cascades stacks
                    // same-position DockLayout siblings in declaration order,
                    // and messageView (declared above) was covering these at
                    // their old position right after the titleBar, right
                    // where the top of the message list also sits.
                    Container {
                        // Shown while a tapped Reel is being fetched (see
                        // messageView.onTriggered below, which sets
                        // reelLoading true when fetchInstagramVideo() didn't
                        // return an already-cached path) -- otherwise a tap
                        // gave zero feedback until the video popped open
                        // (or silently didn't, on failure), which looked
                        // like nothing happened.
                        visible: reelLoading
                        horizontalAlignment: HorizontalAlignment.Center
                        verticalAlignment: VerticalAlignment.Top
                        topMargin: ui.du(2)
                        background: Color.create("#26313d")
                        layout: StackLayout { orientation: LayoutOrientation.LeftToRight }
                        leftPadding: ui.du(1.5); rightPadding: ui.du(1.5)
                        topPadding: ui.du(0.8); bottomPadding: ui.du(0.8)
                        ActivityIndicator {
                            running: reelLoading
                            preferredWidth: ui.du(3); preferredHeight: ui.du(3)
                            rightMargin: ui.du(1)
                        }
                        Label {
                            text: "Loading reel..."
                            textStyle.color: Color.White
                            verticalAlignment: VerticalAlignment.Center
                        }
                    }
                    Container {
                        // Reel fetch failed (Instagram page scraping is
                        // fragile by nature -- see
                        // MediaManager::fetchInstagramVideo()). Stays up
                        // until the next Reel tap rather than auto-hiding
                        // after a delay, to avoid a QML Timer element here.
                        visible: reelError
                        horizontalAlignment: HorizontalAlignment.Center
                        verticalAlignment: VerticalAlignment.Top
                        topMargin: ui.du(2)
                        background: Color.create("#4a2020")
                        layout: StackLayout {}
                        leftPadding: ui.du(1.5); rightPadding: ui.du(1.5)
                        topPadding: ui.du(0.8); bottomPadding: ui.du(0.8)
                        Label {
                            text: "Couldn't load the reel."
                            textStyle.color: Color.White
                        }
                    }

                    // "Attach" menu (📎 tap): a plain overlay rather than a
                    // pushed Sheet page, so it's a one-tap dismiss and never
                    // interrupts the composer's focus/scroll state. Declared
                    // last (see the comment on the reel indicators above) so
                    // it paints on top of everything, including those.
                    Container {
                        visible: attachMenuVisible
                        horizontalAlignment: HorizontalAlignment.Fill
                        verticalAlignment: VerticalAlignment.Fill
                        layout: DockLayout {}
                        onTouch: {
                            if (event.isUp()) attachMenuVisible = false;
                        }
                        // Dimmed backdrop, kept as its own node instead of a
                        // background+opacity on this whole Container -- that
                        // opacity would have cascaded to the panel below too
                        // (a Cascades Container's opacity applies to its
                        // whole subtree), washing out the menu itself along
                        // with the chat behind it instead of just dimming
                        // the backdrop.
                        Container {
                            horizontalAlignment: HorizontalAlignment.Fill
                            verticalAlignment: VerticalAlignment.Fill
                            background: Color.create("#000000")
                            opacity: 0.55
                        }
                        Container {
                            horizontalAlignment: HorizontalAlignment.Fill
                            verticalAlignment: VerticalAlignment.Bottom
                            background: Color.create("#1a2026")
                            layout: StackLayout {}
                            topPadding: ui.du(1); bottomPadding: ui.du(1)
                            Container {
                                layout: StackLayout { orientation: LayoutOrientation.LeftToRight }
                                leftPadding: ui.du(2); rightPadding: ui.du(2)
                                topPadding: ui.du(1); bottomPadding: ui.du(1)
                                onTouch: {
                                    if (!event.isUp()) return;
                                    attachMenuVisible = false;
                                    imagePicker.open();
                                }
                                EmojiIcon { emoji: "📷"; iconSize: ui.du(3); rightMargin: ui.du(1.5); verticalAlignment: VerticalAlignment.Center }
                                Label { text: "Picture"; verticalAlignment: VerticalAlignment.Center; textStyle.color: Color.White; textStyle.base: SystemDefaults.TextStyles.PrimaryText }
                            }
                            Container {
                                layout: StackLayout { orientation: LayoutOrientation.LeftToRight }
                                leftPadding: ui.du(2); rightPadding: ui.du(2)
                                topPadding: ui.du(1); bottomPadding: ui.du(1)
                                onTouch: {
                                    if (!event.isUp()) return;
                                    attachMenuVisible = false;
                                    videoPicker.open();
                                }
                                EmojiIcon { emoji: "🎬"; iconSize: ui.du(3); rightMargin: ui.du(1.5); verticalAlignment: VerticalAlignment.Center }
                                Label { text: "Video"; verticalAlignment: VerticalAlignment.Center; textStyle.color: Color.White; textStyle.base: SystemDefaults.TextStyles.PrimaryText }
                            }
                            Container {
                                layout: StackLayout { orientation: LayoutOrientation.LeftToRight }
                                leftPadding: ui.du(2); rightPadding: ui.du(2)
                                topPadding: ui.du(1); bottomPadding: ui.du(1)
                                onTouch: {
                                    if (!event.isUp()) return;
                                    attachMenuVisible = false;
                                    audioPicker.open();
                                }
                                EmojiIcon { emoji: "🎵"; iconSize: ui.du(3); rightMargin: ui.du(1.5); verticalAlignment: VerticalAlignment.Center }
                                Label { text: "Audio"; verticalAlignment: VerticalAlignment.Center; textStyle.color: Color.White; textStyle.base: SystemDefaults.TextStyles.PrimaryText }
                            }
                            Container {
                                layout: StackLayout { orientation: LayoutOrientation.LeftToRight }
                                leftPadding: ui.du(2); rightPadding: ui.du(2)
                                topPadding: ui.du(1); bottomPadding: ui.du(1)
                                onTouch: {
                                    if (!event.isUp()) return;
                                    attachMenuVisible = false;
                                    filePicker.open();
                                }
                                EmojiIcon { emoji: "📄"; iconSize: ui.du(3); rightMargin: ui.du(1.5); verticalAlignment: VerticalAlignment.Center }
                                Label { text: "File"; verticalAlignment: VerticalAlignment.Center; textStyle.color: Color.White; textStyle.base: SystemDefaults.TextStyles.PrimaryText }
                            }
                        }
                    }

                    // So an incoming (or in-progress) device verification is
                    // reachable from inside a conversation too, not just the
                    // inbox -- see VerificationOverlay.qml.
                    VerificationOverlay {}
                }

                attachedObjects: [
                    FilePicker {
                        id: imagePicker
                        type: FileType.Picture
                        title: "Choose a photo"
                        mode: FilePickerMode.Picker
                        onFileSelected: {
                            if (selectedFiles.length > 0) {
                                messageListModel.sendImage(selectedFiles[0]);
                            }
                        }
                    },
                    FilePicker {
                        id: videoPicker
                        type: FileType.Video
                        title: "Choose a video"
                        mode: FilePickerMode.Picker
                        onFileSelected: {
                            if (selectedFiles.length > 0) {
                                messageListModel.sendVideo(selectedFiles[0]);
                            }
                        }
                    },
                    FilePicker {
                        id: audioPicker
                        type: FileType.Music
                        title: "Choose an audio file"
                        mode: FilePickerMode.Picker
                        onFileSelected: {
                            if (selectedFiles.length > 0) {
                                messageListModel.sendAudioFile(selectedFiles[0]);
                            }
                        }
                    },
                    FilePicker {
                        id: filePicker
                        type: FileType.Document | FileType.Other
                        title: "Choose a file"
                        mode: FilePickerMode.Picker
                        onFileSelected: {
                            if (selectedFiles.length > 0) {
                                messageListModel.sendFile(selectedFiles[0]);
                            }
                        }
                    },
                    AudioRecorder {
                        id: audioRecorder
                    },
                    MediaPlayer {
                        id: chatAudioPlayer
                        // Read chatAudioPlayer.xxx explicitly (matching the pattern in
                        // bb.multimedia's own MediaPlayer.hpp QML example) rather than the
                        // bare signal argument -- Cascades' own docs warn that for some
                        // property-changed signals the argument doesn't reliably reflect the
                        // current value in QML and to re-read the property instead.
                        onMediaStateChanged: {
                            messageListModel.audioIsPlaying = (chatAudioPlayer.mediaState === MediaState.Started);
                            if (chatAudioPlayer.mediaState === MediaState.Stopped) {
                                messageListModel.playingAudioEventId = "";
                                messageListModel.audioPositionMs = 0;
                            }
                        }
                        onPositionChanged: {
                            messageListModel.audioPositionMs = chatAudioPlayer.position;
                        }
                        onDurationChanged: {
                            messageListModel.audioDurationMs = chatAudioPlayer.duration;
                        }
                    }
                ]
            }
        }
    ]

    Page {
        // FreeForm title bar so the menu (manage hidden chats / logout) sits
        // as icon buttons in the same top row as the app name, instead of
        // Page.actions' default bottom action bar.
        titleBar: TitleBar {
            kind: TitleBarKind.FreeForm
            kindProperties: FreeFormTitleBarKindProperties {
                Container {
                    horizontalAlignment: HorizontalAlignment.Fill
                    verticalAlignment: VerticalAlignment.Fill
                    // TitleBar has no "visible" property and TitleBarKind has
                    // no "None" value in this Cascades version (checked the
                    // BBNDK headers directly, same as the Button.background/
                    // ActionItem.visible traps) -- there's no clean way to
                    // remove the bar outright for the logged-out state. This
                    // fakes it instead: match the page's own background
                    // (#101316, not this bar's usual #1a2026) and collapse
                    // the content to zero height, so it reads as "no bar"
                    // rather than an empty strip in a different shade. The
                    // login screen shows its own (larger) logo directly in
                    // its content, making this bar's copy redundant there.
                    background: matrixApi.loggedIn ? Color.create("#1a2026") : Color.create("#101316")
                    preferredHeight: matrixApi.loggedIn ? ui.du(8) : 0
                    layout: StackLayout { orientation: LayoutOrientation.LeftToRight }
                    leftPadding: ui.du(2); rightPadding: ui.du(1)
                    ImageView {
                        // In a LeftToRight StackLayout, horizontalAlignment
                        // controls the CROSS axis (vertical here), not
                        // position along the stack -- giving this element
                        // spaceQuota:1 (to "expand and align left" within
                        // that space) actually stretched it to fill the row
                        // instead. A trailing spacer with the spaceQuota
                        // does the actual push-right-content-to-the-right
                        // job, leaving this at its own natural size flush
                        // left.
                        visible: matrixApi.loggedIn
                        imageSource: "asset:///bbport_logo.png"
                        scalingMethod: ScalingMethod.AspectFit
                        preferredHeight: ui.du(6)
                        verticalAlignment: VerticalAlignment.Center
                    }
                    Container {
                        visible: matrixApi.loggedIn
                        layoutProperties: StackLayoutProperties { spaceQuota: 1 }
                    }
                    ImageButton {
                        defaultImageSource: EmojiMap.emojiAsset("🙈")
                        visible: matrixApi.loggedIn
                        preferredWidth: ui.du(7)
                        onClicked: {
                            var page = manageHiddenChatsPage.createObject();
                            navigationPane.push(page);
                        }
                    }
                    ImageButton {
                        // Power symbol (⏻) isn't part of the Unicode emoji
                        // set twemoji covers -- no bundled image exists for
                        // it -- so a door (🚪, "exit") stands in as the
                        // logout icon instead.
                        defaultImageSource: EmojiMap.emojiAsset("🚪")
                        visible: matrixApi.loggedIn
                        preferredWidth: ui.du(7)
                        onClicked: {
                            while (navigationPane.count() > 1) navigationPane.pop();
                            syncEngine.stop();
                            matrixApi.logout();
                        }
                    }
                }
            }
        }
        Container {
            layout: DockLayout {}
            background: Color.create("#101316")

            // ---------------- Login ----------------
            Container {
                visible: !matrixApi.loggedIn
                layout: StackLayout {}
                horizontalAlignment: HorizontalAlignment.Fill
                verticalAlignment: VerticalAlignment.Center
                leftPadding: ui.du(3); rightPadding: ui.du(3)

                ImageView {
                    imageSource: "asset:///bbport_logo.png"
                    scalingMethod: ScalingMethod.AspectFit
                    preferredHeight: ui.du(16)
                    horizontalAlignment: HorizontalAlignment.Center
                    bottomMargin: ui.du(3)
                }
                Label {
                    text: "Log in with a Matrix account: your Beeper homeserver (e.g. matrix.beeper.com) or any Matrix homeserver of your choice."
                    multiline: true
                    textStyle.color: Color.create("#aeb8c2")
                    bottomMargin: ui.du(2)
                }
                DropDown {
                    id: methodDropDown
                    title: "Login method"
                    Option { text: "Username and password"; value: "password"; selected: true }
                    Option { text: "Existing access token"; value: "token" }
                }
                TextField {
                    id: homeserverField
                    text: "https://matrix.beeper.com"
                    hintText: "https://matrix.beeper.com"
                    topMargin: ui.du(2)
                }
                TextField {
                    id: userField
                    hintText: methodDropDown.selectedValue === "token" ? "User ID (@name:server)" : "Username"
                    topMargin: ui.du(1)
                }
                TextField {
                    id: secretField
                    hintText: methodDropDown.selectedValue === "token" ? "Access token" : "Password"
                    inputMode: TextFieldInputMode.Password
                    topMargin: ui.du(1)
                }
                TextField {
                    id: recoveryKeyField
                    hintText: "Recovery Key (optional, to decrypt old messages)"
                    topMargin: ui.du(1)
                }
                Button {
                    id: loginButton
                    text: matrixApi.busy ? "Logging in..." : "Log in"
                    appearance: ControlAppearance.Plain
                    color: Color.create("#2f5eff")
                    enabled: !matrixApi.busy && homeserverField.text.length > 0 && userField.text.length > 0 && secretField.text.length > 0
                    topMargin: ui.du(2)
                    onClicked: {
                        if (methodDropDown.selectedValue === "token") {
                            matrixApi.loginWithToken(homeserverField.text, userField.text, secretField.text);
                        } else {
                            matrixApi.loginWithPassword(homeserverField.text, userField.text, secretField.text);
                        }
                    }
                }
                Label {
                    text: matrixApi.lastError
                    visible: matrixApi.lastError.length > 0
                    multiline: true
                    topMargin: ui.du(1)
                    textStyle.color: Color.create("#e05c5c")
                }
            }

            // ---------------- Chat list ----------------
            Container {
                visible: matrixApi.loggedIn
                layout: DockLayout {}
                horizontalAlignment: HorizontalAlignment.Fill
                verticalAlignment: VerticalAlignment.Fill

                Container {
                    layout: StackLayout {}
                    leftPadding: ui.du(1.5)
                    rightPadding: ui.du(1.5)


                    Container {
                        visible: !navigationPane.verifyBannerDismissed
                        layout: StackLayout {}
                        topPadding: ui.du(1); bottomPadding: ui.du(1)
                        leftPadding: ui.du(1); rightPadding: ui.du(1)
                        background: Color.create("#26313d")

                        Label {
                            text: "To get bridges (e.g. WhatsApp) to accept BBport's messages, verify this device with another Matrix client of yours (e.g. Beeper Desktop)."
                            multiline: true
                            textStyle.base: SystemDefaults.TextStyles.SmallText
                            textStyle.color: Color.create("#c4ccd4")
                            bottomMargin: ui.du(1)
                        }
                        Container {
                            layout: StackLayout { orientation: LayoutOrientation.LeftToRight }
                            Button {
                                text: "Verify device"
                                appearance: ControlAppearance.Plain
                                color: Color.create("#2f5eff")
                                enabled: !olmCryptoManager.verificationActive
                                layoutProperties: StackLayoutProperties { spaceQuota: 1 }
                                onClicked: olmCryptoManager.startVerification()
                            }
                            Button {
                                text: "Not now"
                                appearance: ControlAppearance.Plain
                                layoutProperties: StackLayoutProperties { spaceQuota: 1 }
                                onClicked: navigationPane.verifyBannerDismissed = true
                            }
                        }
                        // Everything past this point once an attempt is
                        // actually active (status/cancel/incoming
                        // accept-decline/SAS compare) now lives in
                        // VerificationOverlay.qml instead -- nested here it
                        // was unreachable the moment this dismissible
                        // banner was dismissed (or a conversation page was
                        // pushed on top), including an INCOMING request
                        // from someone else, which isn't something the
                        // user chose to hide.
                    }

                    TextField {
                        id: searchField
                        hintText: "Search conversations"
                        onTextChanging: {
                            roomListModel.searchQuery = text;
                        }
                    }
                    Label {
                        text: "CONVERSAZIONI"
                        topMargin: ui.du(1)
                        textStyle.base: SystemDefaults.TextStyles.SmallText
                        textStyle.color: Color.create("#9ab8da")
                    }
                    ListView {
                        id: roomView
                        dataModel: roomListModel.model
                        layoutProperties: StackLayoutProperties { spaceQuota: 1 }
                        onTriggered: {
                            var item = roomListModel.roomAt(indexPath[0]);
                            if (item.roomId && !item.isInvite) {
                                navigationPane.openConversation(item.roomId, item.name);
                            }
                        }

                        listItemComponents: [
                            ListItemComponent {
                                type: ""
                                Container {
                                    preferredWidth: ui.du(73)
                                    horizontalAlignment: HorizontalAlignment.Fill
                                    layout: StackLayout {}
                                    leftPadding: 0; rightPadding: 0; topPadding: ui.du(0.8); bottomPadding: ui.du(0.8)
                                    background: Color.create(ListItemData.isInvite ? "#26313d" : "#1a2026")

                                    Container {
                                        layout: StackLayout { orientation: LayoutOrientation.LeftToRight }
                                        Container {
                                            preferredWidth: ui.du(9); preferredHeight: ui.du(9)
                                            verticalAlignment: VerticalAlignment.Center
                                            background: Color.create("#3b4652")
                                            layout: DockLayout {}
                                            Label {
                                                text: (ListItemData.name && ListItemData.name.length > 0) ? ListItemData.name.charAt(0).toUpperCase() : "?"
                                                visible: !ListItemData.avatarLocalUrl || ListItemData.avatarLocalUrl.length === 0
                                                horizontalAlignment: HorizontalAlignment.Center
                                                verticalAlignment: VerticalAlignment.Center
                                                textStyle.base: SystemDefaults.TextStyles.PrimaryText
                                                textStyle.color: Color.White
                                                textStyle.fontWeight: FontWeight.Bold
                                            }
                                            ImageView {
                                                visible: ListItemData.avatarLocalUrl && ListItemData.avatarLocalUrl.length > 0
                                                imageSource: ListItemData.avatarLocalUrl ? ListItemData.avatarLocalUrl : ""
                                                preferredWidth: ui.du(9); preferredHeight: ui.du(9)
                                                scalingMethod: ScalingMethod.AspectFill
                                            }
                                        }
                                        Container {
                                            preferredWidth: ui.du(63)
                                            leftMargin: ui.du(1)
                                            layout: StackLayout {}
                                            Container {
                                                layout: StackLayout { orientation: LayoutOrientation.LeftToRight }
                                                Label {
                                                    text: ListItemData.name
                                                    layoutProperties: StackLayoutProperties { spaceQuota: 1 }
                                                    textStyle.base: SystemDefaults.TextStyles.PrimaryText
                                                    textStyle.color: Color.White
                                                    textStyle.fontWeight: FontWeight.Bold
                                                }
                                                Container {
                                                    visible: ListItemData.unreadCount > 0
                                                    background: Color.create("#6fb8ff")
                                                    leftPadding: ui.du(0.6); rightPadding: ui.du(0.6)
                                                    Label {
                                                        text: ListItemData.unreadCount > 0 ? ListItemData.unreadCount.toString() : ""
                                                        textStyle.base: SystemDefaults.TextStyles.SmallText
                                                        textStyle.color: Color.create("#101316")
                                                    }
                                                }
                                            }
                                            Container {
                                                visible: !ListItemData.isInvite
                                                layout: StackLayout { orientation: LayoutOrientation.LeftToRight }
                                                Label {
                                                    text: ListItemData.isTyping ? "typing..." : ListItemData.lastBody
                                                    multiline: false
                                                    layoutProperties: StackLayoutProperties { spaceQuota: 1 }
                                                    textStyle.base: SystemDefaults.TextStyles.PrimaryText
                                                    textStyle.color: Color.create(ListItemData.isTyping ? "#6fb8ff" : "#c4ccd4")
                                                }
                                                Label {
                                                    text: ListItemData.lastTs > 0 ? Qt.formatTime(new Date(ListItemData.lastTs), "HH:mm") : ""
                                                    leftMargin: ui.du(1)
                                                    textStyle.base: SystemDefaults.TextStyles.PrimaryText
                                                    textStyle.color: Color.create("#c4ccd4")
                                                }
                                            }
                                            Container {
                                                visible: ListItemData.isInvite
                                                layout: StackLayout { orientation: LayoutOrientation.LeftToRight }
                                                Label {
                                                    text: "Invitation received"
                                                    layoutProperties: StackLayoutProperties { spaceQuota: 1 }
                                                    textStyle.base: SystemDefaults.TextStyles.SmallText
                                                    textStyle.color: Color.create("#aeb8c2")
                                                }
                                                Button {
                                                    text: "Accept"
                                                    appearance: ControlAppearance.Plain
                                                    color: Color.create("#2f5eff")
                                                    preferredWidth: ui.du(14)
                                                    onClicked: roomListModel.acceptInvite(ListItemData.roomId)
                                                }
                                                Button {
                                                    text: "Decline"
                                                    appearance: ControlAppearance.Plain
                                                    color: Color.create("#e05c5c")
                                                    preferredWidth: ui.du(14)
                                                    onClicked: roomListModel.declineInvite(ListItemData.roomId)
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        ]
                    }
                }
                Label {
                    text: matrixApi.loggedIn ? (matrixApi.userId + " · " + matrixApi.homeserver) : "Not connected"
                    horizontalAlignment: HorizontalAlignment.Center
                    verticalAlignment: VerticalAlignment.Bottom
                    bottomMargin: ui.du(1)
                    textStyle.base: SystemDefaults.TextStyles.SmallText
                    textStyle.color: Color.create("#8492a2")
                }

                // Covers the room list until the first /sync (potentially
                // hundreds of historical events across every room) has
                // fully landed -- without this the list used to render
                // empty/half-populated and jump around room-by-room as the
                // initial batch trickled in, right as NotificationManager
                // was (before its own initialSyncDone guard) also firing a
                // notification storm for the exact same backlog.
                Container {
                    visible: matrixApi.loggedIn && !syncEngine.initialSyncDone
                    horizontalAlignment: HorizontalAlignment.Fill
                    verticalAlignment: VerticalAlignment.Fill
                    layout: StackLayout {}
                    background: Color.create("#101316")
                    ActivityIndicator {
                        preferredWidth: ui.du(10); preferredHeight: ui.du(10)
                        horizontalAlignment: HorizontalAlignment.Center
                        verticalAlignment: VerticalAlignment.Center
                        running: parent.visible
                    }
                    Label {
                        text: "Syncing..."
                        horizontalAlignment: HorizontalAlignment.Center
                        topMargin: ui.du(2)
                        textStyle.color: Color.create("#8492a2")
                    }
                }

                VerificationOverlay {}
            }
        }
    }
}
