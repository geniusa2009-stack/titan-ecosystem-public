// TITAN Studio — Apple Pro Hybrid design tokens (semantic; Light + Dark). Original TITAN design.
pragma Singleton
import QtQuick

QtObject {
    // Driven by Main.qml bindings from user preferences.
    property bool dark: false
    property real textScale: 1.0
    property bool reducedMotion: false

    // Brand
    readonly property color accent: "#7098D5"
    readonly property color accentStrong: dark ? "#8DAEE3" : "#5A83C3"
    readonly property color textOnAccent: "#FFFFFF"

    // Surfaces
    readonly property color background: dark ? "#131720" : "#F6F7F9"
    readonly property color surface: dark ? "#191E28" : "#FFFFFF"
    readonly property color surfaceElevated: dark ? "#212734" : "#FFFFFF"
    readonly property color surfaceSecondary: dark ? "#282D37" : "#DDE3ED"
    readonly property color sidebar: dark ? "#161A23" : "#EFF2F6"
    readonly property color toolbar: dark ? "#171C25" : "#FAFBFC"
    readonly property color canvas: dark ? "#10141B" : "#D9DEE6"       // viewport clear / fog colour
    readonly property color canvasSurround: dark ? "#0C0F15" : "#E9ECF1"
    readonly property color field: dark ? "#11151D" : "#F2F4F7"
    readonly property color overlay: dark ? Qt.rgba(0.09, 0.11, 0.15, 0.78) : Qt.rgba(1, 1, 1, 0.80)
    readonly property color scrim: Qt.rgba(0, 0, 0, dark ? 0.5 : 0.25)
    readonly property color grid: dark ? Qt.rgba(1, 1, 1, 0.06) : Qt.rgba(0.08, 0.12, 0.2, 0.11)

    // Text
    readonly property color textPrimary: dark ? "#E7EBF1" : "#1A2130"
    readonly property color textSecondary: dark ? "#A2ABB9" : "#586273"
    readonly property color textTertiary: dark ? "#6C7585" : "#8A93A2"
    readonly property color textDisabled: dark ? "#4B5361" : "#B3BAC5"

    // Lines and states
    readonly property color border: dark ? Qt.rgba(1, 1, 1, 0.075) : Qt.rgba(0.07, 0.1, 0.16, 0.10)
    readonly property color borderStrong: dark ? Qt.rgba(1, 1, 1, 0.14) : Qt.rgba(0.07, 0.1, 0.16, 0.18)
    readonly property color hover: dark ? Qt.rgba(1, 1, 1, 0.045) : Qt.rgba(0.07, 0.1, 0.16, 0.045)
    readonly property color active: dark ? Qt.rgba(1, 1, 1, 0.085) : Qt.rgba(0.07, 0.1, 0.16, 0.085)
    readonly property color selection: dark ? Qt.rgba(0.44, 0.6, 0.84, 0.26) : Qt.rgba(0.44, 0.6, 0.84, 0.18)
    readonly property color selectionText: dark ? "#FFFFFF" : "#16335F"
    readonly property color focus: accent
    readonly property color warning: dark ? "#E2B15A" : "#B4741A"
    readonly property color error: dark ? "#EE7A70" : "#C0402C"
    readonly property color success: dark ? "#6CC089" : "#2D8C52"
    readonly property color axisX: dark ? "#E0766C" : "#C9503F"
    readonly property color axisY: dark ? "#7CC47F" : "#3D9346"
    readonly property color axisZ: dark ? "#6F9BE0" : "#3F70C2"

    // Typography (system UI font; Arabic falls back to the platform's Arabic UI face)
    readonly property string fontFamily: Qt.application.font.family
    readonly property string monoFamily: Qt.platform.os === "osx" ? "Menlo" : (Qt.platform.os === "windows" ? "Consolas" : "monospace")
    function px(n) { return Math.round(n * textScale) }
    readonly property int fontCaption: px(10)
    readonly property int fontSmall: px(11)
    readonly property int fontBody: px(12.5)
    readonly property int fontTitle: px(14)
    readonly property int fontHeadline: px(18)
    readonly property int fontLarge: px(24)
    readonly property int fontDisplay: px(40)

    // Geometry and motion
    readonly property int radiusSmall: 4
    readonly property int radius: 6
    readonly property int radiusLarge: 10
    readonly property int controlHeight: Math.max(24, px(26))
    readonly property int rowHeight: Math.max(22, px(24))
    readonly property int durFast: reducedMotion ? 0 : 110
    readonly property int durMed: reducedMotion ? 0 : 190
}
