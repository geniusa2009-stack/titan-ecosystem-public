// TITAN Studio — compact toolbar button with glyph and tooltip. Original TITAN design.
import QtQuick
import QtQuick.Controls.Basic
import TitanStudio

AbstractButton {
    id: control
    property string glyph: ""
    property string tip: ""
    property bool active: false
    property int size: Theme.controlHeight + 2
    implicitWidth: size
    implicitHeight: size
    hoverEnabled: true
    focusPolicy: Qt.StrongFocus
    Accessible.name: tip
    Accessible.role: Accessible.Button

    contentItem: Glyph {
        text: control.glyph
        size: Theme.px(15)
        color: !control.enabled ? Theme.textDisabled : control.active ? Theme.accent : Theme.textSecondary
    }
    background: Rectangle {
        radius: Theme.radius
        color: control.active ? Theme.selection : control.down ? Theme.active : control.hovered ? Theme.hover : "transparent"
        Behavior on color { ColorAnimation { duration: Theme.durFast } }
        border.width: control.visualFocus ? 2 : 0
        border.color: Theme.focus
    }
    ToolTip.visible: hovered && tip.length > 0
    ToolTip.delay: 500
    ToolTip.text: tip
}
