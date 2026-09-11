import bb.cascades 1.4
import "emojimap.js" as EmojiMap

// Shared message-bubble visuals for main.qml's conversationPage. Pulled out
// of the delegate itself so it can be instantiated twice per row (once
// docked Right + visible: isOutgoing, once docked Left + visible:
// !isOutgoing -- see the single "" ListItemComponent in main.qml) instead
// of choosing left/right via a per-row horizontalAlignment binding on one
// shared instance, which never actually moved the bubble on delegate
// recycle no matter how it was bound (Cascades doesn't seem to re-run
// layout for a rebound horizontalAlignment within a recycled
// ListItemComponent, unlike the plain visible/text/background bindings
// used throughout this file, which all update correctly). "visible" toggling
// two statically-docked instances sidesteps the whole question.
Container {
    maxWidth: ui.du(58)
    layout: StackLayout {}
    // Stickers get no bubble chrome at all (matches Element/every other
    // Matrix client): they're typically small, often semi-transparent PNGs
    // meant to sit directly on the chat background, not inside a colored
    // rectangle sized for a paragraph of text. A sticker with a reply-quote
    // or a group sender-name row above it still gets those (unaffected --
    // only this container's own background/padding is skipped).
    leftPadding: ListItemData.msgtype === "m.sticker" ? 0 : ui.du(1.2)
    rightPadding: ListItemData.msgtype === "m.sticker" ? 0 : ui.du(1.2)
    topPadding: ListItemData.msgtype === "m.sticker" ? 0 : ui.du(0.8)
    bottomPadding: ListItemData.msgtype === "m.sticker" ? 0 : ui.du(0.8)
    // Outgoing gets a deep brand-blue tint (rather than a same-family gray
    // as before) so "your messages" reads as a deliberate color choice tied
    // to the app's own icon blue, not just a slightly lighter panel.
    background: ListItemData.msgtype === "m.sticker" ? Color.Transparent : Color.create(ListItemData.isOutgoing ? "#1e3a66" : "#252c33")

    // Reaction pills are a fixed number of slots (no Repeater -- Cascades'
    // QML1 doesn't support QtQuick's dynamic item generation inside a
    // Container, since its children must be VisualNodes, not plain Items)
    // rather than one label listing every reaction as text, so each key can
    // be shown as its actual emoji image instead of unreadable tofu boxes.
    function reactionKey(i) {
        var r = ListItemData.reactions;
        return (r && i < r.length) ? r[i].key : "";
    }
    function reactionCount(i) {
        var r = ListItemData.reactions;
        return (r && i < r.length) ? r[i].count : "";
    }

    Container {
        visible: !ListItemData.isOutgoing && ListItemData.isGroupChat
        layout: StackLayout { orientation: LayoutOrientation.LeftToRight }
        ImageView {
            visible: ListItemData.senderAvatarLocalUrl && ListItemData.senderAvatarLocalUrl.length > 0
            imageSource: ListItemData.senderAvatarLocalUrl ? ListItemData.senderAvatarLocalUrl : ""
            preferredWidth: ui.du(3); preferredHeight: ui.du(3)
            rightMargin: ui.du(0.5)
            scalingMethod: ScalingMethod.AspectFill
        }
        Label {
            text: ListItemData.senderShort
            textStyle.base: SystemDefaults.TextStyles.SmallText
            textStyle.color: Color.create("#9ab8da")
            textStyle.fontWeight: FontWeight.Bold
        }
    }
    Container {
        visible: ListItemData.replyPreview && ListItemData.replyPreview.length > 0
        leftPadding: ui.du(0.8)
        bottomMargin: ui.du(0.4)
        background: Color.create(ListItemData.isOutgoing ? "#16294d" : "#1c2228")
        layout: DockLayout {}
        Label {
            text: ListItemData.replyPreview
            multiline: true
            textStyle.base: SystemDefaults.TextStyles.BodyText
            textStyle.color: Color.create("#9ab8da")
            leftMargin: ui.du(0.6)
            topMargin: ui.du(0.3); bottomMargin: ui.du(0.3)
        }
    }
    Container {
        visible: (ListItemData.msgtype === "m.image" || ListItemData.msgtype === "m.sticker") && ListItemData.mediaMxc && ListItemData.mediaMxc.length > 0
        layout: DockLayout {}
        // Sized to the photo's own aspect ratio (content.info.w/h --
        // ListItemData.mediaWidth/mediaHeight, the same fields the video
        // letterbox calc below uses) instead of a fixed box. A fixed
        // 40x30du box squeezed every portrait photo into a landscape frame
        // (or pillarboxed a landscape one) with big empty margins either
        // way -- every other Matrix client sizes the bubble to the image.
        property real maxBoxW: ListItemData.msgtype === "m.sticker" ? ui.du(22) : ui.du(46)
        property real maxBoxH: ListItemData.msgtype === "m.sticker" ? ui.du(22) : ui.du(46)
        // Floor so a very wide/short (or very tall/narrow) image doesn't
        // collapse to a sliver on the axis the aspect-ratio scale shrinks
        // hardest -- ImageView's own AspectFit below just letterboxes the
        // extra space on that axis rather than stretching, so this never
        // distorts the image.
        property real minBox: ui.du(12)
        preferredWidth: {
            var w = ListItemData.mediaWidth, h = ListItemData.mediaHeight;
            if (w <= 0 || h <= 0) return maxBoxW;
            var scale = Math.min(maxBoxW / w, maxBoxH / h);
            return Math.max(minBox, Math.round(w * scale));
        }
        preferredHeight: {
            var w = ListItemData.mediaWidth, h = ListItemData.mediaHeight;
            if (w <= 0 || h <= 0) return maxBoxH;
            var scale = Math.min(maxBoxW / w, maxBoxH / h);
            return Math.max(minBox, Math.round(h * scale));
        }
        ImageView {
            imageSource: ListItemData.mediaLocalUrl && ListItemData.mediaLocalUrl.length > 0 ? ListItemData.mediaLocalUrl : ""
            horizontalAlignment: HorizontalAlignment.Fill
            verticalAlignment: VerticalAlignment.Fill
            scalingMethod: ScalingMethod.AspectFit
            // Tap handling lives on messageView.onTriggered, not
            // here: neither onTouch nor a nested TapHandler fired
            // reliably from inside a ListItemComponent delegate in
            // this Cascades build (a known-tricky scope/gesture
            // interaction with the list's own touch handling), and
            // ListView::triggered() is the documented, reliable way
            // to react to a tap on a row.
        }
    }
    Container {
        visible: (ListItemData.msgtype === "m.video" || ListItemData.msgtype === "m.audio" || ListItemData.msgtype === "m.file") && ListItemData.mediaMxc && ListItemData.mediaMxc.length > 0
        layout: StackLayout { orientation: LayoutOrientation.LeftToRight }
        topMargin: ui.du(0.4); bottomMargin: ui.du(0.4)
        EmojiIcon {
            visible: ListItemData.msgtype !== "m.audio" || !ListItemData.mediaLocalUrl || ListItemData.mediaLocalUrl.length === 0
            emoji: ListItemData.msgtype === "m.video" ? "🎬" : (ListItemData.msgtype === "m.audio" ? "🎵" : "📄")
            iconSize: ui.du(3)
            rightMargin: ui.du(0.8)
        }
        Container {
            // Root-caused via the QML debug console: a
            // ListItemComponent delegate does NOT inherit
            // main.qml's document context in this Cascades
            // build, so "messageListModel" (or any other context
            // property) throws "ReferenceError: Can't find
            // variable" from in here -- confirmed on-device, not
            // a theory. ListItemData (this row's own QVariantMap)
            // is the only channel proven to reach the delegate,
            // so playback state is now written into the row's
            // own item by MessageListModel::patchAudioPlaybackItem
            // (C++) instead of read from a context property.
            // Blue = tap to play, orange = playing (tap to
            // pause) -- tap is handled by messageView.onTriggered
            // at Page scope, which CAN reach messageListModel.
            visible: ListItemData.msgtype === "m.audio" && ListItemData.mediaLocalUrl && ListItemData.mediaLocalUrl.length > 0
            preferredWidth: ui.du(4)
            preferredHeight: ui.du(4)
            background: ListItemData.isPlayingAudio ? Color.create("#e08a3c") : Color.create("#2f5eff")
            rightMargin: ui.du(0.8)
        }
        Label {
            // Folded into this already-proven-rendering Label
            // (used to just show the raw filename) rather than a
            // separate new element, to keep this simple now that
            // the real bug (context property unreachable from the
            // delegate) is fixed at the source.
            text: {
                var isAudio = ListItemData.msgtype === "m.audio" && ListItemData.mediaLocalUrl && ListItemData.mediaLocalUrl.length > 0;
                var playing = ListItemData.isPlayingAudio ? true : false;
                var posMs = ListItemData.audioLivePositionMs ? ListItemData.audioLivePositionMs : 0;
                var durMs = (ListItemData.audioLiveDurationMs && ListItemData.audioLiveDurationMs > 0) ? ListItemData.audioLiveDurationMs : ListItemData.mediaDuration;
                var posSec = Math.floor(posMs / 1000);
                var durSec = Math.floor(durMs / 1000);
                var posMm = Math.floor(posSec / 60);
                var posSs = posSec % 60;
                var durMm = Math.floor(durSec / 60);
                var durSs = durSec % 60;
                var posStr = posMm + ":" + (posSs < 10 ? "0" : "") + posSs;
                var durStr = durMm + ":" + (durSs < 10 ? "0" : "") + durSs;
                var audioText = (playing ? "[Pausa] " : "[Play] ") + posStr + " / " + durStr;
                return isAudio ? audioText : ListItemData.body;
            }
            multiline: true
            layoutProperties: StackLayoutProperties { spaceQuota: 1 }
            textStyle.base: SystemDefaults.TextStyles.PrimaryText
            textStyle.color: Color.create(ListItemData.isOutgoing ? "#ffffff" : "#f1f3f5")
        }
    }
    Container {
        // Scrub bar: read-only progress display, driven purely
        // by ListItemData (see above) -- dragging it can't
        // forward a seek request back to chatAudioPlayer since
        // that's Page-scope and unreachable from here, so there's
        // no onValueChanged handler; seek-by-drag would need a
        // different mechanism (e.g. a row tap gesture at
        // Page/ListView scope) if wanted later.
        visible: ListItemData.msgtype === "m.audio" && ListItemData.mediaLocalUrl && ListItemData.mediaLocalUrl.length > 0
        layout: StackLayout { orientation: LayoutOrientation.LeftToRight }
        bottomMargin: ui.du(0.4)
        Slider {
            enabled: false
            layoutProperties: StackLayoutProperties { spaceQuota: 1 }
            fromValue: 0
            toValue: {
                var d = (ListItemData.audioLiveDurationMs && ListItemData.audioLiveDurationMs > 0) ? ListItemData.audioLiveDurationMs : ListItemData.mediaDuration;
                return d > 0 ? d : 1;
            }
            value: ListItemData.audioLivePositionMs ? ListItemData.audioLivePositionMs : 0
        }
    }
    Label {
        text: {
            if (!ListItemData.mediaMxc || ListItemData.mediaMxc.length === 0) return "";
            if (ListItemData.mediaLocalUrl && ListItemData.mediaLocalUrl.length > 0) return "";
            return ListItemData.mediaDownloadFailed ? "Couldn't download." : "Downloading...";
        }
        visible: text.length > 0
        textStyle.base: SystemDefaults.TextStyles.SmallText
        textStyle.color: Color.create(ListItemData.mediaDownloadFailed ? "#e08080" : "#aeb8c2")
    }
    Label {
        text: ListItemData.msgtype === "m.text" ? ListItemData.body : ""
        visible: text.length > 0
        multiline: true
        content.flags: TextContentFlag.ActiveText
        textStyle.base: SystemDefaults.TextStyles.PrimaryText
        // Deleted messages (see TimelineStore::onEventRedacted() /
        // SyncEngine's redacted_because handling) read as muted + italic,
        // the same visual language WhatsApp/Telegram/Signal use for "this
        // message was deleted" -- distinct from a normal message at a
        // glance, not just different text.
        textStyle.fontStyle: ListItemData.isRedacted ? FontStyle.Italic : FontStyle.Default
        textStyle.color: Color.create(ListItemData.isRedacted ? "#8492a2" : (ListItemData.isOutgoing ? "#ffffff" : "#f1f3f5"))
    }
    Container {
        // Up to 6 reaction pills (a generous cap for the common case of a
        // handful of distinct reaction keys on a message), each its own
        // emoji image + count -- see reactionKey()/reactionCount() above.
        visible: ListItemData.reactions && ListItemData.reactions.length > 0
        layout: StackLayout { orientation: LayoutOrientation.LeftToRight }
        topMargin: ui.du(0.4)

        Container {
            visible: reactionKey(0).length > 0
            layout: StackLayout { orientation: LayoutOrientation.LeftToRight }
            rightMargin: ui.du(0.7)
            EmojiIcon { emoji: reactionKey(0); iconSize: ui.du(2.2) }
            Label { text: String(reactionCount(0)); leftMargin: ui.du(0.2); verticalAlignment: VerticalAlignment.Center; textStyle.base: SystemDefaults.TextStyles.SmallText; textStyle.color: Color.create("#e4e8ec") }
        }
        Container {
            visible: reactionKey(1).length > 0
            layout: StackLayout { orientation: LayoutOrientation.LeftToRight }
            rightMargin: ui.du(0.7)
            EmojiIcon { emoji: reactionKey(1); iconSize: ui.du(2.2) }
            Label { text: String(reactionCount(1)); leftMargin: ui.du(0.2); verticalAlignment: VerticalAlignment.Center; textStyle.base: SystemDefaults.TextStyles.SmallText; textStyle.color: Color.create("#e4e8ec") }
        }
        Container {
            visible: reactionKey(2).length > 0
            layout: StackLayout { orientation: LayoutOrientation.LeftToRight }
            rightMargin: ui.du(0.7)
            EmojiIcon { emoji: reactionKey(2); iconSize: ui.du(2.2) }
            Label { text: String(reactionCount(2)); leftMargin: ui.du(0.2); verticalAlignment: VerticalAlignment.Center; textStyle.base: SystemDefaults.TextStyles.SmallText; textStyle.color: Color.create("#e4e8ec") }
        }
        Container {
            visible: reactionKey(3).length > 0
            layout: StackLayout { orientation: LayoutOrientation.LeftToRight }
            rightMargin: ui.du(0.7)
            EmojiIcon { emoji: reactionKey(3); iconSize: ui.du(2.2) }
            Label { text: String(reactionCount(3)); leftMargin: ui.du(0.2); verticalAlignment: VerticalAlignment.Center; textStyle.base: SystemDefaults.TextStyles.SmallText; textStyle.color: Color.create("#e4e8ec") }
        }
        Container {
            visible: reactionKey(4).length > 0
            layout: StackLayout { orientation: LayoutOrientation.LeftToRight }
            rightMargin: ui.du(0.7)
            EmojiIcon { emoji: reactionKey(4); iconSize: ui.du(2.2) }
            Label { text: String(reactionCount(4)); leftMargin: ui.du(0.2); verticalAlignment: VerticalAlignment.Center; textStyle.base: SystemDefaults.TextStyles.SmallText; textStyle.color: Color.create("#e4e8ec") }
        }
        Container {
            visible: reactionKey(5).length > 0
            layout: StackLayout { orientation: LayoutOrientation.LeftToRight }
            EmojiIcon { emoji: reactionKey(5); iconSize: ui.du(2.2) }
            Label { text: String(reactionCount(5)); leftMargin: ui.du(0.2); verticalAlignment: VerticalAlignment.Center; textStyle.base: SystemDefaults.TextStyles.SmallText; textStyle.color: Color.create("#e4e8ec") }
        }
    }
    Container {
        layout: StackLayout { orientation: LayoutOrientation.LeftToRight }
        horizontalAlignment: HorizontalAlignment.Right
        Label {
            // Deliberately more muted than the surrounding text (a
            // timestamp is metadata, not content -- it shouldn't compete
            // for attention with the message itself or the sender name).
            text: ListItemData.timeText
            textStyle.base: SystemDefaults.TextStyles.SmallText
            textStyle.color: Color.create("#7c8794")
        }
        EmojiIcon {
            emoji: "✔"
            visible: ListItemData.isOutgoing && ListItemData.readBy && ListItemData.readBy.length > 0
            iconSize: ui.du(1.8)
            leftMargin: ui.du(0.3)
            verticalAlignment: VerticalAlignment.Center
        }
    }
}
