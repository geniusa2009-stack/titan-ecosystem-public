// TITAN Studio — XYZ editor row. Drag an axis label horizontally to scrub the value; a
// whole scrub gesture is coalesced into one undo step by the engine.
import QtQuick
import QtQuick.Layouts
import TitanStudio

ColumnLayout {
    id: root
    property string label: ""
    property var value: [0, 0, 0]
    property var mixed: [false, false, false]  // per axis: the selected objects differ
    property real step: 0.1
    property int decimals: 3
    property string fieldPrefix: ""
    signal edited(int axis, real v, string coalesce)
    spacing: 4

    Text {
        text: root.label
        color: Theme.textSecondary
        font.pixelSize: Theme.fontSmall
        Layout.fillWidth: true
    }
    RowLayout {
        spacing: 6
        Layout.fillWidth: true
        LayoutMirroring.enabled: false
        LayoutMirroring.childrenInherit: true
        Repeater {
            model: 3
            delegate: RowLayout {
                id: cell
                required property int index
                spacing: 0
                Layout.fillWidth: true
                Rectangle {
                    implicitWidth: 18
                    implicitHeight: Theme.controlHeight
                    radius: Theme.radiusSmall
                    color: scrub.pressed ? Theme.active : "transparent"
                    Text {
                        anchors.centerIn: parent
                        text: ["X", "Y", "Z"][cell.index]
                        font.pixelSize: Theme.fontSmall
                        font.weight: Font.DemiBold
                        color: [Theme.axisX, Theme.axisY, Theme.axisZ][cell.index]
                    }
                    MouseArea {
                        id: scrub
                        anchors.fill: parent
                        cursorShape: Qt.SizeHorCursor
                        property real startX: 0
                        property real startValue: 0
                        property int session: 0
                        onPressed: (m) => { startX = m.x; startValue = root.mixed[cell.index] ? 0 : root.value[cell.index]; session++ }
                        onPositionChanged: (m) => {
                            const k = (m.modifiers & Qt.ShiftModifier) ? 10 : 1
                            const v = startValue + Math.round((m.x - startX) / 3) * root.step * k
                            root.edited(cell.index, v, "scrub-" + root.fieldPrefix + cell.index + "-" + session)
                        }
                    }
                }
                NumberField {
                    objectName: root.fieldPrefix + ["X", "Y", "Z"][cell.index]
                    Layout.fillWidth: true
                    Layout.minimumWidth: 52
                    value: root.value[cell.index]
                    mixed: !!root.mixed[cell.index]
                    step: root.step
                    decimals: root.decimals
                    Accessible.name: root.label + " " + ["X", "Y", "Z"][cell.index]
                    onCommitted: (v, key) => root.edited(cell.index, v, key)
                }
            }
        }
    }
}
