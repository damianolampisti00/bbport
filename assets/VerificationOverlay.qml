import bb.cascades 1.4

// A device-verification request/response can arrive at any moment, on
// whichever screen happens to be open -- previously this whole UI lived
// only inside the inbox's own dismissible advisory banner
// (Container{visible: !verifyBannerDismissed} in main.qml), which meant an
// incoming request (or the SAS comparison step of one already in progress)
// was completely unreachable once that banner had been dismissed, or
// whenever a conversation page was pushed on top of the inbox. Instantiated
// as the LAST child of each page's own DockLayout (inbox and
// conversationPage both do this), so it always paints on top regardless of
// which page is currently showing -- same "declare it last" convention used
// for the sync spinner/attach menu/reel-loading banner elsewhere in
// main.qml.
Container {
    visible: olmCryptoManager.verificationActive
    horizontalAlignment: HorizontalAlignment.Fill
    verticalAlignment: VerticalAlignment.Fill
    layout: DockLayout {}

    Container {
        horizontalAlignment: HorizontalAlignment.Fill
        verticalAlignment: VerticalAlignment.Fill
        background: Color.create("#000000")
        opacity: 0.6
    }

    Container {
        horizontalAlignment: HorizontalAlignment.Center
        verticalAlignment: VerticalAlignment.Center
        preferredWidth: ui.du(48)
        background: Color.create("#1a2026")
        layout: StackLayout {}
        leftPadding: ui.du(2); rightPadding: ui.du(2)
        topPadding: ui.du(2); bottomPadding: ui.du(2)

        Label {
            text: olmCryptoManager.verificationIncomingPending
                    ? "Verification request from " + olmCryptoManager.verificationIncomingFrom
                    : "Device verification"
            multiline: true
            textStyle.base: SystemDefaults.TextStyles.TitleText
            textStyle.color: Color.White
            bottomMargin: ui.du(1)
        }
        Label {
            text: olmCryptoManager.verificationStatus
            visible: text.length > 0
            multiline: true
            bottomMargin: ui.du(1)
            textStyle.base: SystemDefaults.TextStyles.SmallText
            textStyle.color: Color.create("#9ab8da")
        }
        Container {
            visible: olmCryptoManager.verificationIncomingPending
            layout: StackLayout { orientation: LayoutOrientation.LeftToRight }
            Button {
                text: "Accept request"
                appearance: ControlAppearance.Plain
                color: Color.create("#2f5eff")
                layoutProperties: StackLayoutProperties { spaceQuota: 1 }
                onClicked: olmCryptoManager.acceptIncomingVerification()
            }
            Button {
                text: "Decline"
                appearance: ControlAppearance.Plain
                color: Color.create("#e05c5c")
                layoutProperties: StackLayoutProperties { spaceQuota: 1 }
                onClicked: olmCryptoManager.cancelVerification()
            }
        }
        Label {
            text: olmCryptoManager.verificationSas
            visible: olmCryptoManager.verificationAwaitingConfirm
            horizontalAlignment: HorizontalAlignment.Center
            topMargin: ui.du(1)
            textStyle.base: SystemDefaults.TextStyles.TitleText
            textStyle.color: Color.White
        }
        Label {
            text: olmCryptoManager.verificationEmoji
            visible: olmCryptoManager.verificationAwaitingConfirm
            multiline: true
            horizontalAlignment: HorizontalAlignment.Center
            topMargin: ui.du(1)
            textStyle.color: Color.White
        }
        Container {
            visible: olmCryptoManager.verificationAwaitingConfirm
            layout: StackLayout { orientation: LayoutOrientation.LeftToRight }
            topMargin: ui.du(1)
            Button {
                text: "They match"
                appearance: ControlAppearance.Plain
                color: Color.create("#2f5eff")
                layoutProperties: StackLayoutProperties { spaceQuota: 1 }
                onClicked: olmCryptoManager.confirmVerification()
            }
            Button {
                text: "They don't match"
                appearance: ControlAppearance.Plain
                color: Color.create("#e05c5c")
                layoutProperties: StackLayoutProperties { spaceQuota: 1 }
                onClicked: olmCryptoManager.cancelVerification()
            }
        }
        Button {
            text: "Cancel verification"
            appearance: ControlAppearance.Plain
            color: Color.create("#e05c5c")
            visible: !olmCryptoManager.verificationIncomingPending && !olmCryptoManager.verificationAwaitingConfirm
            topMargin: ui.du(1)
            onClicked: olmCryptoManager.cancelVerification()
        }
    }
}
