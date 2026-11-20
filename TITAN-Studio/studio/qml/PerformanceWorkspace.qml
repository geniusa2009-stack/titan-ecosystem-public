// TITAN Studio — Performance workspace: measured values only (viewport, IPC, engine host).
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import TitanStudio

Rectangle {
    id: root
    property var viewport: null
    property var liveSwitch: null
    property var status: ({})
    color: Theme.background

    Timer {
        interval: 1000
        repeat: true
        running: root.visible && engineClient.connected
        triggeredOnStart: true
        onTriggered: engineClient.request("engine.status", {}, (r, err) => { if (!err) root.status = r })
    }

    function fmt(v, d, unit) { return (v === undefined || v === null || isNaN(v)) ? "—" : Number(v).toFixed(d) + (unit ? " " + unit : "") }

    component MetricRow: RowLayout {
        property string label
        property string value
        property string note: ""
        Layout.fillWidth: true
        spacing: 12
        Text { text: parent.label; color: Theme.textSecondary; font.pixelSize: Theme.fontBody; Layout.preferredWidth: 220 }
        Text { text: parent.value; color: Theme.textPrimary; font.pixelSize: Theme.fontBody; font.family: Theme.monoFamily; Layout.preferredWidth: 160; LayoutMirroring.enabled: false }
        Text { text: parent.note; color: Theme.textTertiary; font.pixelSize: Theme.fontSmall; Layout.fillWidth: true; elide: Text.ElideRight }
    }
    component Group: Rectangle {
        default property alias content: inner.data
        property string title
        Layout.fillWidth: true
        implicitHeight: inner.implicitHeight + 28
        radius: Theme.radiusLarge
        color: Theme.surface
        border.color: Theme.border
        ColumnLayout {
            id: inner
            x: 16; y: 14
            width: parent.width - 32
            spacing: 8
            SectionHeader { title: parent.parent.title; Layout.fillWidth: true }
        }
    }

    Flickable {
        anchors.fill: parent
        contentHeight: col.implicitHeight + 48
        clip: true
        ColumnLayout {
            id: col
            x: 32; y: 24
            width: Math.min(parent.width - 64, 900)
            spacing: 16
            RowLayout {
                Layout.fillWidth: true
                Text { text: Strings.t("Performance"); color: Theme.textPrimary; font.pixelSize: Theme.fontLarge; font.weight: Font.DemiBold }
                Item { Layout.fillWidth: true }
                Text { text: Strings.t("Live rendering"); color: Theme.textSecondary; font.pixelSize: Theme.fontSmall }
                Switch {
                    id: live
                    checked: root.liveSwitch ? root.liveSwitch.checked : false
                    onToggled: if (root.liveSwitch) root.liveSwitch.checked = checked
                }
            }
            Text {
                text: "All values are measured on this machine. Viewport timings are updated while the World workspace is visible; with live rendering off the viewport renders on demand, so frame intervals are not meaningful and are not shown."
                color: Theme.textTertiary; font.pixelSize: Theme.fontSmall; wrapMode: Text.Wrap; Layout.fillWidth: true
            }
            Group {
                title: "Viewport (TITAN Studio renderer)"
                MetricRow { label: "Graphics backend"; value: root.viewport ? root.viewport.backendName : "—"; note: "QRhi" }
                MetricRow { label: "Frame interval"; value: root.viewport && root.viewport.frameMs > 0 ? root.fmt(root.viewport.frameMs, 2, "ms") : "—"; note: root.viewport && root.viewport.frameMs > 0 ? root.fmt(root.viewport.fps, 0, "fps (vsync-limited)") : "live rendering off" }
                MetricRow { label: "Render CPU time"; value: root.viewport ? root.fmt(root.viewport.cpuRenderMs, 3, "ms") : "—"; note: "command recording, last frame" }
                MetricRow { label: "GPU time"; value: root.viewport && root.viewport.gpuMs > 0 ? root.fmt(root.viewport.gpuMs, 3, "ms") : "n/a"; note: "QRhi timestamps (if supported by backend)" }
                MetricRow { label: "Draw calls"; value: root.viewport ? String(root.viewport.drawCalls) : "—" }
                MetricRow { label: "Frames rendered"; value: root.viewport ? String(Math.round(root.viewport.frameCount)) : "—" }
            }
            Group {
                title: "Editor ↔ engine protocol"
                MetricRow { label: "Engine start → connected"; value: root.fmt(engineClient.connectMs, 0, "ms") }
                MetricRow { label: "Last round trip"; value: root.fmt(engineClient.lastRttMs, 3, "ms") }
                MetricRow { label: "Average round trip"; value: root.fmt(engineClient.avgRttMs, 3, "ms"); note: "request written → reply read on Studio's I/O thread" }
                MetricRow { label: "Average GUI delivery"; value: root.fmt(engineClient.avgDeliveryMs, 3, "ms"); note: "… → reply handled on the UI thread" }
                MetricRow { label: "Project open (engine)"; value: root.fmt(engineClient.lastOpenMs, 2, "ms") }
                MetricRow { label: "Scene save (engine)"; value: root.fmt(engineClient.lastSaveMs, 2, "ms"); note: "atomic write + fsync" }
            }
            Group {
                id: ueGroup  // children are reparented into Group's column, so "parent" is not the Group
                title: "Unreal Engine renderer"
                visible: root.status.unreal !== undefined
                property var u: root.status.unreal || ({})
                MetricRow { label: "Engine / RHI"; value: (ueGroup.u.engineVersion || "—"); note: ueGroup.u.rhi || "" }
                MetricRow { label: "Engine frame"; value: root.fmt(ueGroup.u.frameMs, 2, "ms"); note: "game " + root.fmt(ueGroup.u.gameThreadMs, 2) + " · render " + root.fmt(ueGroup.u.renderThreadMs, 2) + " · RHI " + root.fmt(ueGroup.u.rhiThreadMs, 2) + " ms" }
                MetricRow { label: "Engine GPU frame"; value: root.fmt(ueGroup.u.gpuMs, 2, "ms"); note: "RHIGetGPUFrameCycles" }
                MetricRow { label: "Nanite · Lumen · VSM"; value: (ueGroup.u["r.Nanite"] || "?") + " · " + (ueGroup.u["r.DynamicGlobalIlluminationMethod"] || "?") + " · " + (ueGroup.u["r.Shadow.Virtual.Enable"] || "?"); note: "r.Nanite · r.DynamicGlobalIlluminationMethod (1 = Lumen) · r.Shadow.Virtual.Enable" }
                MetricRow { label: "Shaders compiling"; value: ueGroup.u.shadersRemaining !== undefined ? String(ueGroup.u.shadersRemaining) : "—" }
                MetricRow { label: "Frames captured / delivered"; value: ueGroup.u.frames ? Math.round(ueGroup.u.frames.captured) + " / " + Math.round(ueGroup.u.frames.delivered) : "—"; note: ueGroup.u.frames ? Math.round(ueGroup.u.frames.droppedBusy) + " skipped while the GPU was busy" : "" }
                MetricRow { label: "Capture → pixels in shared memory"; value: ueGroup.u.frames ? root.fmt(ueGroup.u.frames.avgCaptureToReadyMs, 2, "ms") : "—"; note: "inside Unreal (render + async readback)" }
                MetricRow { label: "Capture → shown in Studio"; value: root.viewport && root.viewport.showingEngineFrames ? root.fmt(root.viewport.engineFrameLatencyMs, 2, "ms") : "—"; note: root.viewport ? root.viewport.engineFrameSize + " px, " + Math.round(root.viewport.engineFrameCount) + " frames shown" : "" }
                MetricRow { label: "Engine memory in use"; value: ueGroup.u.usedPhysicalBytes ? root.fmt(ueGroup.u.usedPhysicalBytes / 1048576, 0, "MiB") : "—" }
            }
            Group {
                title: "Engine host process"
                MetricRow { label: "Resident memory"; value: root.status.rssBytes ? root.fmt(root.status.rssBytes / 1048576, 1, "MiB") : "—" }
                MetricRow { label: "Uptime"; value: root.status.uptimeMs ? root.fmt(root.status.uptimeMs / 1000, 0, "s") : "—" }
                MetricRow { label: "Objects in scene"; value: root.status.objectCount !== undefined ? String(root.status.objectCount) : "—" }
                MetricRow { label: "Requests handled"; value: root.status.metrics ? String(root.status.metrics.requests) : "—"; note: root.status.metrics ? root.status.metrics.errors + " errors" : "" }
                MetricRow { label: "Avg / max request time"; value: root.status.metrics ? root.fmt(root.status.metrics.avgRequestUs, 1) + " / " + root.fmt(root.status.metrics.maxRequestUs, 0, "µs") : "—"; note: "inside the engine, excludes IPC" }
            }
        }
    }
}
