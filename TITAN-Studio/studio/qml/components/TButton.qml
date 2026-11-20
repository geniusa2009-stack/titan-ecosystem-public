// TITAN Studio — push button. Original TITAN design.
import QtQuick
import QtQuick.Controls.Basic
import TitanStudio

Button {
    id: control
    property bool primary: false
    property bool destructive: false
    property bool subtle: false

    implicitHeight: Theme.controlHeight
    implicitWidth: Math.max(64, contentItem.implicitWidth + 24)
    hoverEnabled: true
    focusPolicy: Qt.StrongFocus
    font.pixelSize: Theme.fontBody
    font.weight: primary ? Font.DemiBold : Font.Medium

    contentItem: Text {
        text: control.text
        font: control.font
        color: !control.enabled ? Theme.textDisabled
             : control.primary ? Theme.textOnAccent
             : control.destructive ? Theme.error : Theme.textPrimary
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }
    background: Rectangle {
        radius: Theme.radius
        color: control.primary
               ? (control.enabled ? (control.down ? Qt.darker(Theme.accent, 1.15) : control.hovered ? Theme.accentStrong : Theme.accent) : Theme.surfaceSecondary)
               : control.subtle ? (control.down ? Theme.active : control.hovered ? Theme.hover : "transparent")
               : (control.down ? Theme.active : control.hovered ? Theme.surfaceElevated : Theme.surface)
        border.width: control.primary || control.subtle ? 0 : 1
        border.color: Theme.borderStrong
        Behavior on color { ColorAnimation { duration: Theme.durFast } }
        Rectangle {  // focus ring
            anchors.fill: parent
            anchors.margins: -3
            radius: Theme.radius + 3
            color: "transparent"
            border.width: 2
            border.color: Theme.focus
            visible: control.visualFocus
        }
    }
}
