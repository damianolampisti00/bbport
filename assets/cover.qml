import bb.cascades 1.4

// Active Frame content shown on the Home screen when BBport is minimized
// (BB10's "cover mode"). Non-interactive and unanimated, per SceneCover's
// own contract -- just a live label bound to roomListModel.totalUnreadCount.
Container {
    background: Color.create("#1c1c1c")
    horizontalAlignment: HorizontalAlignment.Fill
    verticalAlignment: VerticalAlignment.Fill
    layout: DockLayout {}

    Container {
        horizontalAlignment: HorizontalAlignment.Center
        verticalAlignment: VerticalAlignment.Center
        layout: StackLayout {
            orientation: LayoutOrientation.TopToBottom
        }

        ImageView {
            // A separate copy of the repo-root icon.png, not the same file
            // bar-descriptor.xml's <asset path="icon.png"> packages for the
            // launcher icon -- that one lands at native/icon.png, outside
            // the assets/ folder entirely, so "asset:///" (scoped to
            // native/assets/, same as every other asset:/// reference in
            // this app, e.g. bbport_logo.png) could never find it there.
            // Confirmed broken on a real device: AssetPathResolver logged
            // "Did not find any asset ... filePath=icon.png" and the cover
            // rendered with no icon at all.
            imageSource: "asset:///icon.png"
            horizontalAlignment: HorizontalAlignment.Center
            preferredWidth: 64
            preferredHeight: 64
        }

        Label {
            text: "BBport"
            horizontalAlignment: HorizontalAlignment.Center
            textStyle.color: Color.White
            textStyle.fontSize: FontSize.Small
            topMargin: 6
        }

        Label {
            text: roomListModel.totalUnreadCount > 0
                    ? (roomListModel.totalUnreadCount > 99 ? "99+" : roomListModel.totalUnreadCount)
                    : "0"
            horizontalAlignment: HorizontalAlignment.Center
            textStyle.color: roomListModel.totalUnreadCount > 0 ? Color.create("#4fc3f7") : Color.create("#666666")
            textStyle.fontWeight: FontWeight.Bold
            textStyle.fontSize: FontSize.XXLarge
            topMargin: 4
        }

        Label {
            text: roomListModel.totalUnreadCount > 0 ? "unread messages" : "no messages"
            horizontalAlignment: HorizontalAlignment.Center
            textStyle.color: Color.create("#999999")
            textStyle.fontSize: FontSize.XSmall
        }
    }
}
