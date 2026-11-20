// TITAN Studio — Command Palette (⌘K).
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import TitanStudio

Popup {
    id: pop
    property var commands: []
    property var filtered: []
    parent: Overlay.overlay
    x: (parent.width - width) / 2
    y: 80
    width: Math.min(600, parent.width - 80)
    height: Math.min(440, 64 + list.contentHeight)
    modal: true
    focus: true
    padding: 0
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    Overlay.modal: Rectangle { color: Theme.scrim }
    enter: Transition { NumberAnimation { property: "opacity"; from: 0; to: 1; duration: Theme.durFast } }

    function openPalette() { field.text = ""; refilter(); open(); field.forceActiveFocus() }
    function refilter() {
        const q = field.text.toLowerCase().trim()
        filtered = commands.filter(c => q.length === 0 || Strings.t(c.title).toLowerCase().indexOf(q) >= 0 || c.title.toLowerCase().indexOf(q) >= 0)
        list.currentIndex = 0
    }
    function runCurrent() {
        const c = filtered[list.currentIndex]
        if (!c || !c.enabled()) return
        close()
        c.run()
    }

    background: Rectangle { color: Theme.surfaceElevated; radius: Theme.radiusLarge; border.color: Theme.borderStrong }
    contentItem: ColumnLayout {
        spacing: 0
        TTextField {
            id: field
            Layout.fillWidth: true
            Layout.margins: 10
            implicitHeight: 34
            font.pixelSize: Theme.fontTitle
            placeholderText: Strings.t("Type a command…")
            onTextChanged: pop.refilter()
            Keys.onDownPressed: list.incrementCurrentIndex()
            Keys.onUpPressed: list.decrementCurrentIndex()
            Keys.onReturnPressed: pop.runCurrent()
            Keys.onEnterPressed: pop.runCurrent()
        }
        Rectangle { Layout.fillWidth: true; height: 1; color: Theme.border }
        ListView {
            id: list
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.margins: 6
            clip: true
            model: pop.filtered
            highlightMoveDuration: 0
            delegate: Rectangle {
                id: item
                required property var modelData
                required property int index
                readonly property bool on: modelData.enabled()
                width: list.width
                height: 30
                radius: Theme.radius
                color: list.currentIndex === index ? Theme.selection : hov.hovered ? Theme.hover : "transparent"
                HoverHandler { id: hov }
                TapHandler { onTapped: { list.currentIndex = item.index; pop.runCurrent() } }
                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 10
                    anchors.rightMargin: 10
                    Text { text: Strings.t(item.modelData.title); color: item.on ? Theme.textPrimary : Theme.textDisabled; font.pixelSize: Theme.fontBody; Layout.fillWidth: true; elide: Text.ElideRight }
                    Text { text: item.modelData.shortcut; color: Theme.textTertiary; font.pixelSize: Theme.fontSmall; font.family: Theme.monoFamily }
                }
            }
        }
    }
}
