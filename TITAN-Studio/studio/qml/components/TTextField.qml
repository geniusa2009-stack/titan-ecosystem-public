// TITAN Studio — text field. Original TITAN design.
import QtQuick
import QtQuick.Controls.Basic
import TitanStudio

TextField {
    id: control
    property bool mono: false
    implicitHeight: Theme.controlHeight
    font.pixelSize: Theme.fontBody
    font.family: mono ? Theme.monoFamily : Theme.fontFamily
    color: enabled ? Theme.textPrimary : Theme.textDisabled
    placeholderTextColor: Theme.textTertiary
    selectionColor: Theme.selection
    selectedTextColor: Theme.selectionText
    leftPadding: 8
    rightPadding: 8
    verticalAlignment: TextInput.AlignVCenter
    selectByMouse: true
    background: Rectangle {
        radius: Theme.radius
        color: Theme.field
        border.width: control.activeFocus ? 2 : 1
        border.color: control.activeFocus ? Theme.focus : Theme.border
        Behavior on border.color { ColorAnimation { duration: Theme.durFast } }
    }
}
