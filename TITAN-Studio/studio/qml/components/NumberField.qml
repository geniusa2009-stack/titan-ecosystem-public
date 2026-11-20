// TITAN Studio — numeric field: Enter/blur commits, ↑/↓ nudges (Shift ×10), Esc reverts.
// Nudges share a coalesce key so a burst of arrow presses is one undo step.
import QtQuick
import TitanStudio

TTextField {
    id: field
    property real value: 0
    property real step: 0.1
    property int decimals: 3
    property real minimum: -1e9
    property real maximum: 1e9
    property string coalesceKey: ""
    // Multi-object editing: the selected objects disagree on this value. The field shows "Mixed" and stays
    // empty until the user types a value (leaving it empty changes nothing).
    property bool mixed: false
    placeholderText: mixed ? "Mixed" : ""
    signal committed(real v, string coalesce)

    mono: true
    horizontalAlignment: TextInput.AlignRight
    LayoutMirroring.enabled: false  // numbers stay LTR in RTL layouts
    text: mixed ? "" : format(value)
    inputMethodHints: Qt.ImhFormattedNumbersOnly
    // Built from a JS string so "\u200E" is the LRM character itself: as a regex literal the escape reached
    // QRegularExpression (PCRE2) verbatim, which rejects \uXXXX, so the validator was invalid and refused all
    // typed input ("called on an invalid QRegularExpression object").
    validator: RegularExpressionValidator { regularExpression: new RegExp("^\u200E?-?\\d*\\.?\\d*(e-?\\d+)?$") }

    // A leading LEFT-TO-RIGHT MARK pins numbers to LTR even when the active keyboard or
    // UI language is right-to-left (otherwise "-1.5" can render as "1.5-").
    readonly property string lrm: "\u200E"
    function format(v) {
        const s = Number(v).toFixed(decimals)
        return lrm + (decimals > 0 ? s.replace(/\.?0+$/, "") : s)
    }
    function parse(t) { return parseFloat(String(t).replace(/\u200E/g, "")) }
    function clamp(v) { return Math.min(maximum, Math.max(minimum, v)) }
    // Public: commit a value programmatically (also used by the UI smoke test).
    function commitValue(v) {
        if (mixed && String(text).replace(/\u200E/g, "").trim() === "") return  // still mixed: nothing to apply
        const n = clamp(Number(v))
        if (!isFinite(n)) { text = format(value); return }
        text = format(n)
        if (mixed || Math.abs(n - value) > 1e-9) committed(n, coalesceKey)
    }
    onValueChanged: if (!activeFocus) text = mixed ? "" : format(value)
    onMixedChanged: if (!activeFocus) text = mixed ? "" : format(value)
    onEditingFinished: commitValue(parse(text))
    onActiveFocusChanged: if (activeFocus) selectAll()
    Keys.onEscapePressed: { text = format(value); focus = false }
    Keys.onUpPressed: (e) => nudge(e.modifiers & Qt.ShiftModifier ? 10 : 1)
    Keys.onDownPressed: (e) => nudge(e.modifiers & Qt.ShiftModifier ? -10 : -1)
    property int nudgeSession: 0
    Timer { id: nudgeIdle; interval: 800; onTriggered: field.nudgeSession++ }
    function nudge(k) {
        const n = clamp((parse(text) || 0) + k * step)
        text = format(n)
        nudgeIdle.restart()
        committed(n, "nudge-" + objectName + "-" + nudgeSession)
    }
}
