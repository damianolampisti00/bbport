import bb.cascades 1.4
import "emojimap.js" as EmojiMap

// BB10's system font only covers a small slice of Unicode emoji -- most
// render as an empty box. This shows the bundled twemoji PNG for a given
// emoji character/sequence when one exists (assets/emoji/, see
// assets/emojimap.js), falling back to the raw text glyph otherwise (native
// emoji, or any symbol -- e.g. plain typographic marks -- that isn't
// actually part of the Unicode emoji set twemoji covers).
Container {
    property string emoji: ""
    property real iconSize: ui.du(3)
    property color fallbackColor: Color.White
    property string resolvedAsset: EmojiMap.emojiAsset(emoji)

    layout: DockLayout {}

    ImageView {
        visible: resolvedAsset.length > 0
        imageSource: resolvedAsset
        preferredWidth: iconSize
        preferredHeight: iconSize
        scalingMethod: ScalingMethod.AspectFit
    }
    Label {
        visible: resolvedAsset.length === 0
        text: emoji
        textStyle.base: SystemDefaults.TextStyles.TitleText
        textStyle.color: fallbackColor
    }
}
