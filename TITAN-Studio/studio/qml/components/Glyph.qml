// TITAN Studio — glyph icon (Unicode geometric shapes from system fonts; no proprietary icon sets).
import QtQuick
import TitanStudio

Text {
    property int size: Theme.px(14)
    font.pixelSize: size
    color: Theme.textSecondary
    horizontalAlignment: Text.AlignHCenter
    verticalAlignment: Text.AlignVCenter
    renderType: Text.NativeRendering
}
