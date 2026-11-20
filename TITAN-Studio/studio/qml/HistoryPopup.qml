// TITAN Studio — undo history: the engine's existing undo stack (edit.history), not a second history.
// Rows are the applied edits (oldest first) and the redoable ones (dimmed); clicking a row jumps there with
// edit.goto, which undoes or redoes one step at a time. Fetched only while the popup is open.
import QtQuick
import QtQuick.Controls.Basic
import TitanStudio

Popup {
    id: root
    objectName: "historyPopup"
    width: 280
    padding: 6
    property var entries: []
    property int position: 0
    property int saved: -1
    property string error: ""
    function reload() {
        engineClient.request("edit.history", {}, (r, err) => {
            if (err) { error = err.message; return }
            error = ""
            entries = r.entries
            position = r.position
            saved = r.saved
        })
    }
    function jump(p) {
        if (p === position) return
        engineClient.request("edit.goto", { position: p }, (r, err) => {
            if (err) engineClient.notify(err.message, "error")
            root.reload()
        })
    }
    onOpened: reload()
    Connections {
        target: engineClient
        enabled: root.opened
        function onStateChanged() { root.reload() }
    }
    background: Rectangle { color: Theme.surface; border.color: Theme.border; radius: Theme.radiusLarge }
    contentItem: Column {
        spacing: 2
        Text {
            text: root.error !== "" ? root.error : root.entries.length + " steps · at " + root.position
            color: root.error !== "" ? Theme.error : Theme.textTertiary; font.pixelSize: Theme.fontCaption
            leftPadding: 8; bottomPadding: 4
        }
        ListView {
            id: list
            objectName: "historyList"
            width: parent.width
            height: Math.min(contentHeight, 360)
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            // Row 0 is the state before any recorded edit; row i (1..n) is the state after entry i-1.
            model: root.entries.length + 1
            delegate: Rectangle {
                id: row
                required property int index
                readonly property bool current: index === root.position
                readonly property bool redoable: index > root.position
                objectName: "historyRow:" + index
                width: list.width; height: Theme.rowHeight; radius: Theme.radiusSmall
                color: current ? Theme.selection : hov.hovered ? Theme.hover : "transparent"
                HoverHandler { id: hov }
                TapHandler { onTapped: root.jump(row.index) }
                Text {
                    anchors.left: parent.left; anchors.leftMargin: 8; anchors.right: mark.left; anchors.verticalCenter: parent.verticalCenter
                    text: row.index === 0 ? "Opened project" : root.entries[row.index - 1]
                    elide: Text.ElideRight
                    color: row.current ? Theme.selectionText : row.redoable ? Theme.textDisabled : Theme.textPrimary
                    font.pixelSize: Theme.fontSmall
                    font.italic: row.index === 0
                }
                Text {
                    id: mark
                    anchors.right: parent.right; anchors.rightMargin: 8; anchors.verticalCenter: parent.verticalCenter
                    visible: row.index === root.saved
                    text: "saved"; color: Theme.textTertiary; font.pixelSize: Theme.fontCaption
                }
            }
            onCountChanged: positionViewAtIndex(root.position, ListView.Contain)
        }
    }
}
