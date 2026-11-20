// TITAN Studio — panel section header.
import QtQuick
import QtQuick.Layouts
import TitanStudio

RowLayout {
    property string title: ""
    property string detail: ""
    spacing: 6
    Text {
        text: parent.title.toUpperCase()
        color: Theme.textTertiary
        font.pixelSize: Theme.fontCaption
        font.weight: Font.DemiBold
        font.letterSpacing: 0.8
    }
    Item { Layout.fillWidth: true }
    Text {
        text: parent.detail
        color: Theme.textTertiary
        font.pixelSize: Theme.fontCaption
        visible: text.length > 0
    }
}
