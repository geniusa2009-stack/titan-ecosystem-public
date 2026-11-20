// TITAN Studio — segmented control.
import QtQuick
import QtQuick.Controls.Basic
import TitanStudio

Rectangle {
    id: root
    property var model: []
    property int currentIndex: 0
    signal activated(int index)
    implicitHeight: Theme.controlHeight
    implicitWidth: row.implicitWidth + 4
    radius: Theme.radius
    color: Theme.dark ? Theme.field : Theme.surfaceSecondary
    border.width: 1
    border.color: Theme.border

    Row {
        id: row
        anchors.fill: parent
        anchors.margins: 2
        Repeater {
            model: root.model
            delegate: AbstractButton {
                id: seg
                required property int index
                required property var modelData
                width: Math.max(implicitWidth, (root.width - 4) / Math.max(1, root.model.length))
                height: row.height
                implicitWidth: label.implicitWidth + 20
                hoverEnabled: true
                focusPolicy: Qt.StrongFocus
                checked: root.currentIndex === index
                onClicked: { root.currentIndex = index; root.activated(index) }
                Accessible.name: String(modelData)
                contentItem: Text {
                    id: label
                    text: String(seg.modelData)
                    font.pixelSize: Theme.fontSmall
                    font.weight: seg.checked ? Font.DemiBold : Font.Normal
                    color: seg.checked ? Theme.textPrimary : Theme.textSecondary
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                    elide: Text.ElideRight
                }
                background: Rectangle {
                    radius: Theme.radius - 2
                    color: seg.checked ? (Theme.dark ? Theme.surfaceSecondary : Theme.surface) : seg.hovered ? Theme.hover : "transparent"
                    border.width: seg.visualFocus ? 2 : 0
                    border.color: Theme.focus
                    Behavior on color { ColorAnimation { duration: Theme.durFast } }
                }
            }
        }
    }
}
