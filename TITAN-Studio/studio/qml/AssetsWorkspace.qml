// TITAN Studio — Assets workspace: the project's asset pipeline as reported by the engine
// (assets.list / assets.refresh, docs/ASSET_PIPELINE.md). Engines that do not advertise the "assets"
// capability get an explicit "not provided" state, never an empty or simulated list.
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import TitanStudio

Rectangle {
    id: root
    objectName: "assetsWorkspace"
    color: Theme.background
    property var assets: []
    property var stats: null
    property string error: ""
    property bool busy: false
    property string filter: ""
    property int kindIndex: 0
    readonly property var kinds: ["All", "Textures", "Models", "Audio", "Prefabs"]
    readonly property var kindKeys: ["", "texture", "model", "audio", "prefab"]
    readonly property bool supported: engineClient.capabilities.indexOf("assets") >= 0
    readonly property var shown: assets.filter(a =>
        (filter === "" || a.path.toLowerCase().indexOf(filter.toLowerCase()) >= 0) &&
        (kindIndex === 0 || ((a.metadata || {}).kind === kindKeys[kindIndex])))
    // Thumbnail source for textures that imported successfully (read straight from the project's file).
    function previewUrl(a) {
        if (a.status !== "ok" || (a.metadata || {}).kind !== "texture" || !engineClient.projectPath) return ""
        return "file://" + engineClient.projectPath + "/Assets/" + a.path.split("/").map(encodeURIComponent).join("/")
    }
    function pathOf(id) {
        const hit = assets.find(x => x.id === id)
        return hit ? hit.path : id.substring(0, 12)
    }

    function handle(r, err) {
        busy = false
        if (err) { error = err.message || "request failed"; return }
        error = ""
        assets = r.assets
        if (r.stats) stats = r.stats
    }
    function reload() {
        if (!supported || !engineClient.projectOpen) return
        busy = true
        engineClient.request("assets.list", {}, handle)
    }
    function refresh() {
        if (!supported || !engineClient.projectOpen) return
        busy = true
        engineClient.request("assets.refresh", {}, handle)
    }
    // Instantiates a prefab asset into the open scene (one undoable step) and selects the new root.
    function place(path) {
        engineClient.request("prefab.instantiate", { path: path }, (r, err) => {
            if (err) { engineClient.notify("Could not place " + path + ": " + err.message, "error"); return }
            engineClient.select([r.root])
            engineClient.notify("Placed " + path + " (" + r.ids.length + " objects)")
        })
    }
    function summary(a) {
        const m = a.metadata || {}
        if (m.kind === "texture") return m.width + "×" + m.height + " · " + m.channels + " ch · " + m.format
        if (m.kind === "audio") return (m.sampleRate / 1000) + " kHz · " + m.channels + " ch · " + Number(m.durationSeconds).toFixed(2) + " s"
        if (m.kind === "model") return m.meshes + " meshes · " + m.materials + " materials · " + m.format
        if (m.kind === "buffer") return Math.round(m.bytes) + " bytes"
        if (m.kind === "prefab") return "prefab · " + m.objects + " objects"
        return ""
    }
    onVisibleChanged: if (visible) reload()
    Connections {
        target: engineClient
        function onStateChanged() { if (root.visible) root.reload() }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 24
        spacing: 12
        RowLayout {
            Layout.fillWidth: true
            Text { text: "Assets"; color: Theme.textPrimary; font.pixelSize: Theme.fontLarge; font.weight: Font.DemiBold }
            Text {
                visible: root.stats !== null
                text: root.stats ? (root.stats.imported + " imported · " + root.stats.reused + " reused · " + root.stats.failed + " failed · " + root.stats.missing + " missing") : ""
                color: Theme.textTertiary; font.pixelSize: Theme.fontSmall; font.family: Theme.monoFamily
            }
            Item { Layout.fillWidth: true }
            Segmented { objectName: "assetsKind"; model: root.kinds; currentIndex: root.kindIndex; onActivated: (i) => root.kindIndex = i; enabled: root.supported }
            TTextField { objectName: "assetsFilter"; placeholderText: "Filter by path"; Layout.preferredWidth: 220; onTextChanged: root.filter = text; enabled: root.supported }
            TButton { objectName: "assetsRefresh"; text: root.busy ? "Importing…" : "Import changes"; enabled: root.supported && engineClient.projectOpen && !root.busy; onClicked: root.refresh() }
        }
        Text {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            color: Theme.textTertiary; font.pixelSize: Theme.fontSmall
            text: !root.supported
                  ? "The connected engine (" + (engineClient.engineInfo.backend || engineClient.engineKind) + ") does not provide the TITAN asset pipeline yet."
                  : "Sources live in <project>/Assets. Unchanged files are reused from the cache; a changed file re-imports itself and the assets that read it. A failed import keeps the last good result."
        }
        Text { visible: root.error !== ""; text: root.error; color: Theme.error; font.pixelSize: Theme.fontSmall; Layout.fillWidth: true; wrapMode: Text.Wrap }
        ListView {
            id: list
            objectName: "assetsList"
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            spacing: 6
            model: root.shown
            delegate: Rectangle {
                required property var modelData
                width: ListView.view.width
                implicitHeight: Math.max(col.implicitHeight, thumb.visible ? 56 : 0) + 20
                radius: Theme.radiusLarge
                color: Theme.surface
                border.color: Theme.border
                Image {
                    id: thumb
                    objectName: "assetThumbnail"
                    x: 14; y: 10
                    width: source != "" ? 56 : 0; height: 56
                    visible: source != ""
                    source: root.previewUrl(modelData)
                    sourceSize: Qt.size(112, 112)
                    fillMode: Image.PreserveAspectFit
                    asynchronous: true
                    cache: false  // a re-import must show the new pixels
                }
                ColumnLayout {
                    id: col
                    x: thumb.visible ? 82 : 14; y: 10
                    width: parent.width - x - 14
                    spacing: 4
                    RowLayout {
                        Layout.fillWidth: true
                        Text { text: modelData.path; color: Theme.textPrimary; font.pixelSize: Theme.fontBody; font.weight: Font.DemiBold; elide: Text.ElideMiddle; Layout.fillWidth: true }
                        Text { text: root.summary(modelData); color: Theme.textSecondary; font.pixelSize: Theme.fontSmall; font.family: Theme.monoFamily }
                        TButton {
                            objectName: "placePrefab"
                            visible: (modelData.metadata || {}).kind === "prefab" && modelData.status === "ok" && engineClient.capabilities.indexOf("prefabs") >= 0
                            text: "Place"; subtle: true
                            onClicked: root.place(modelData.path)
                        }
                        Rectangle {
                            implicitWidth: st.implicitWidth + 14; implicitHeight: 20; radius: 10
                            color: modelData.status === "ok" ? Qt.rgba(0.3, 0.7, 0.45, 0.16) : Qt.rgba(0.85, 0.35, 0.3, 0.16)
                            Text { id: st; anchors.centerIn: parent; text: modelData.status; font.pixelSize: Theme.fontCaption; font.weight: Font.DemiBold; color: modelData.status === "ok" ? Theme.success : Theme.error }
                        }
                    }
                    Text {
                        text: modelData.importer + " · " + modelData.id.substring(0, 12) + (modelData.dependencies.length ? " · reads " + modelData.dependencies.map(root.pathOf).join(", ") : "")
                        color: Theme.textTertiary; font.pixelSize: Theme.fontCaption; font.family: Theme.monoFamily
                    }
                    Text { visible: !!modelData.error; text: modelData.error || ""; color: Theme.error; font.pixelSize: Theme.fontSmall; wrapMode: Text.Wrap; Layout.fillWidth: true }
                }
            }
            Text {
                anchors.centerIn: parent
                visible: root.supported && root.assets.length === 0 && !root.busy
                text: engineClient.projectOpen ? "No assets yet: add files to <project>/Assets and choose Import changes." : "Open a project to see its assets."
                color: Theme.textTertiary; font.pixelSize: Theme.fontSmall
            }
        }
    }
}
