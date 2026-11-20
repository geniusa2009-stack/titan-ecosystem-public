// TITAN Studio — shell for workspaces that are planned but not implemented in this release.
// Deliberately explicit: nothing here pretends to work.
import QtQuick
import QtQuick.Layouts
import TitanStudio

Rectangle {
    id: ph
    property string title
    property string glyph
    property string summary
    property var planned: []
    color: Theme.background

    ColumnLayout {
        anchors.centerIn: parent
        width: Math.min(parent.width - 80, 520)
        spacing: 10
        Glyph { text: ph.glyph; size: Theme.px(34); color: Theme.textTertiary; Layout.alignment: Qt.AlignHCenter }
        Text { text: Strings.t(ph.title); color: Theme.textPrimary; font.pixelSize: Theme.fontLarge; font.weight: Font.DemiBold; Layout.alignment: Qt.AlignHCenter }
        Text { text: ph.summary; color: Theme.textSecondary; font.pixelSize: Theme.fontBody; wrapMode: Text.Wrap; horizontalAlignment: Text.AlignHCenter; Layout.fillWidth: true }
        Rectangle {
            Layout.alignment: Qt.AlignHCenter
            implicitWidth: badge.implicitWidth + 20; implicitHeight: 24; radius: 12
            color: Qt.rgba(0.85, 0.65, 0.25, 0.14)
            Text { id: badge; anchors.centerIn: parent; text: Strings.t("Planned — not available in TITAN Studio Alpha") + " " + titanVersion; color: Theme.warning; font.pixelSize: Theme.fontSmall; font.weight: Font.Medium }
        }
        Item { Layout.preferredHeight: 6 }
        Repeater {
            model: ph.planned
            delegate: Text {
                required property string modelData
                text: "·  " + modelData
                color: Theme.textTertiary
                font.pixelSize: Theme.fontSmall
                Layout.alignment: Qt.AlignHCenter
            }
        }
    }
}
