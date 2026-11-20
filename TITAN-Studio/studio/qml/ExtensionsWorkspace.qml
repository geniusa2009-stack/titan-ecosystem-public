// TITAN Studio — Extensions workspace (installed extensions as reported by the engine host).
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import TitanStudio

Rectangle {
    id: root
    color: Theme.background
    property var exts: []
    property string platform: ""
    readonly property bool manageable: engineClient.capabilities.indexOf("extensions.manage") >= 0
    function apply(r, err) {
        if (err) { engineClient.notify(err.message, "error"); return }
        root.exts = r.extensions; root.platform = r.platform
    }
    function reload() { engineClient.request("extensions.list", {}, apply) }
    // Rescans the extension folders without restarting the engine (new, updated or removed extensions).
    function rescan() { engineClient.request("extensions.reload", {}, apply) }
    // Persists the choice; a disabled extension is listed but never loaded, and its dependents fail.
    function setEnabled(id, on) { engineClient.request("extensions.setEnabled", { id: id, enabled: on }, apply) }
    onVisibleChanged: if (visible) reload()
    Component.onCompleted: if (engineClient.connected) reload()

    ColumnLayout {
        x: 32; y: 24
        width: Math.min(parent.width - 64, 900)
        spacing: 14
        RowLayout {
            Layout.fillWidth: true
            Text { text: Strings.t("Extensions"); color: Theme.textPrimary; font.pixelSize: Theme.fontLarge; font.weight: Font.DemiBold }
            Item { Layout.fillWidth: true }
            Text { text: "Host platform " + root.platform; color: Theme.textTertiary; font.pixelSize: Theme.fontSmall; font.family: Theme.monoFamily }
            TButton { objectName: "extensionsRescan"; text: "Rescan"; subtle: true; visible: root.manageable; onClicked: root.rescan() }
            TButton { text: "Refresh"; subtle: true; onClicked: root.reload() }
        }
        Text {
            text: "Native extensions run inside the engine host process. Manifests are validated (API version, engine range, platform, dependencies, capabilities) before loading, but a manifest is a compatibility contract, not a sandbox: only reviewed extensions should be installed. The public Marketplace and Private Partner Hub are not part of this release."
            color: Theme.textTertiary; font.pixelSize: Theme.fontSmall; wrapMode: Text.Wrap; Layout.fillWidth: true
        }
        Repeater {
            model: root.exts
            delegate: Rectangle {
                required property var modelData
                Layout.fillWidth: true
                implicitHeight: info.implicitHeight + 28
                radius: Theme.radiusLarge
                color: Theme.surface
                border.color: Theme.border
                ColumnLayout {
                    id: info
                    x: 16; y: 14
                    width: parent.width - 32
                    spacing: 6
                    RowLayout {
                        Layout.fillWidth: true
                        Text { text: modelData.name || modelData.id; color: Theme.textPrimary; font.pixelSize: Theme.fontTitle; font.weight: Font.DemiBold }
                        Text { text: modelData.version; color: Theme.textTertiary; font.pixelSize: Theme.fontSmall; font.family: Theme.monoFamily }
                        Item { Layout.fillWidth: true }
                        TButton {
                            objectName: "extensionToggle"
                            visible: root.manageable
                            text: modelData.enabled === false ? "Enable" : "Disable"
                            subtle: true
                            onClicked: root.setEnabled(modelData.id, modelData.enabled === false)
                        }
                        Rectangle {
                            implicitWidth: st.implicitWidth + 14; implicitHeight: 20; radius: 10
                            color: modelData.status === "loaded" ? Qt.rgba(0.3, 0.7, 0.45, 0.16) : Qt.rgba(0.85, 0.35, 0.3, 0.16)
                            Text { id: st; anchors.centerIn: parent; text: modelData.status; font.pixelSize: Theme.fontCaption; font.weight: Font.DemiBold; color: modelData.status === "loaded" ? Theme.success : Theme.error }
                        }
                    }
                    Text { text: modelData.id + " · " + modelData.publisher + " · " + modelData.category + " · " + modelData.license; color: Theme.textSecondary; font.pixelSize: Theme.fontSmall; font.family: Theme.monoFamily; Layout.fillWidth: true; elide: Text.ElideRight }
                    Text { text: "Capabilities: " + (modelData.capabilities || []).join(", "); color: Theme.textTertiary; font.pixelSize: Theme.fontSmall }
                    Text { visible: (modelData.problems || []).length > 0; text: "Problems: " + (modelData.problems || []).join("; "); color: Theme.error; font.pixelSize: Theme.fontSmall; wrapMode: Text.Wrap; Layout.fillWidth: true }
                }
            }
        }
    }
}
