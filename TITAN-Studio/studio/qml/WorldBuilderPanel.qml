// TITAN Studio — World Builder panel: recipe parameters → preview (ghosts) → approve/discard.
// The form is generated from the extension's own `describe` schema.
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import TitanStudio

Item {
    id: root
    property var viewport: null
    readonly property string extensionId: "titan.procedural.scatter"
    property var schema: []
    property var values: ({})
    property string editingRecipe: ""      // empty = new recipe
    property var proposal: null
    property string status: ""
    property bool busy: false

    function loadSchema() {
        if (!engineClient.connected) return
        engineClient.request("extensions.invoke", { extension: extensionId, command: "describe" }, (r, err) => {
            if (err) { status = "Extension unavailable: " + err.message; return }
            schema = r.params
            const v = {}
            for (const p of r.params) v[p.name] = p.default
            values = v
        })
    }
    function setValue(name, v) { const c = Object.assign({}, values); c[name] = v; values = c }
    function editRecipe(r) {
        editingRecipe = r.id
        values = Object.assign({}, values, r.params)
        discard()
    }
    function preview() {
        busy = true
        const p = { extension: extensionId, recipe: values }
        if (editingRecipe) p.recipeId = editingRecipe
        engineClient.request("worldbuilder.preview", p, (r, err) => {
            busy = false
            if (err) { status = err.message; proposal = null; if (viewport) viewport.clearPreview(); return }
            proposal = r
            if (viewport) viewport.setPreview(r.objects)
            status = r.produced + " objects proposed" + (r.replaces.length ? " · replaces " + r.replaces.length : "") + (r.warning ? " · " + r.warning : "")
        })
    }
    function approve() {
        if (!proposal) return
        busy = true
        engineClient.request("worldbuilder.approve", { proposalId: proposal.proposalId }, (r, err) => {
            busy = false
            if (viewport) viewport.clearPreview()
            proposal = null
            if (err) { status = err.message; return }
            editingRecipe = r.recipeId
            status = "Committed " + r.created.length + " objects as " + r.recipeId + " (one undo step)"
            engineClient.notify(status, "success")
        })
    }
    function discard() {
        if (proposal) engineClient.request("worldbuilder.reject", { proposalId: proposal.proposalId })
        proposal = null
        if (viewport) viewport.clearPreview()
        status = ""
    }

    Component.onCompleted: loadSchema()
    Connections { target: engineClient; function onConnectionChanged() { if (engineClient.connected && root.schema.length === 0) root.loadSchema() } }
    Connections { target: engineClient; function onProjectLoaded() { root.editingRecipe = ""; root.proposal = null; root.status = "" } }

    Flickable {
        anchors.fill: parent
        contentHeight: col.implicitHeight + 24
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ScrollBar {}
        ColumnLayout {
            id: col
            x: 14; y: 12
            width: parent.width - 28
            spacing: 12

            Text { text: "Procedural Scatter"; color: Theme.textPrimary; font.pixelSize: Theme.fontTitle; font.weight: Font.DemiBold }
            Text {
                text: "Deterministic, recipe-driven generation. Previews are not part of the scene until you approve them; approval is a single undoable step, and the recipe is saved with the scene so it can be regenerated selectively."
                color: Theme.textSecondary; font.pixelSize: Theme.fontSmall; wrapMode: Text.Wrap; Layout.fillWidth: true
            }

            SectionHeader { title: "Recipe"; detail: root.editingRecipe || Strings.t("New recipe"); Layout.fillWidth: true }
            Flow {
                Layout.fillWidth: true
                spacing: 6
                TButton { text: Strings.t("New recipe"); subtle: root.editingRecipe !== ""; primary: false; onClicked: { root.editingRecipe = ""; root.discard() } }
                Repeater {
                    model: engineClient.recipes
                    delegate: TButton {
                        required property var modelData
                        text: modelData.id
                        subtle: root.editingRecipe !== modelData.id
                        onClicked: root.editRecipe(modelData)
                    }
                }
            }

            SectionHeader { title: "Parameters"; Layout.fillWidth: true }
            Repeater {
                model: root.schema
                delegate: RowLayout {
                    required property var modelData
                    Layout.fillWidth: true
                    spacing: 8
                    Text { text: modelData.label; color: Theme.textSecondary; font.pixelSize: Theme.fontSmall; Layout.preferredWidth: 104; elide: Text.ElideRight }
                    NumberField {
                        visible: modelData.type !== "enum"
                        Layout.fillWidth: true
                        value: root.values[modelData.name] !== undefined ? root.values[modelData.name] : 0
                        step: modelData.type === "int" ? 1 : 0.1
                        decimals: modelData.type === "int" ? 0 : 2
                        minimum: modelData.min !== undefined ? modelData.min : -1e9
                        maximum: modelData.max !== undefined ? modelData.max : 1e9
                        onCommitted: (v) => root.setValue(modelData.name, modelData.type === "int" ? Math.round(v) : v)
                    }
                    Segmented {
                        visible: modelData.type === "enum"
                        Layout.fillWidth: true
                        model: modelData.options || []
                        currentIndex: (modelData.options || []).indexOf(root.values[modelData.name])
                        onActivated: (i) => root.setValue(modelData.name, modelData.options[i])
                    }
                }
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                TButton { text: "Randomize seed"; subtle: true; onClicked: root.setValue("seed", Math.floor(Math.random() * 100000)) }
                Item { Layout.fillWidth: true }
            }

            Rectangle { Layout.fillWidth: true; height: 1; color: Theme.border }
            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                TButton { text: Strings.t("Preview"); primary: !root.proposal; enabled: !root.busy && root.schema.length > 0 && engineClient.projectOpen; onClicked: root.preview() }
                TButton { text: Strings.t("Approve"); primary: !!root.proposal; enabled: !!root.proposal && !root.busy; onClicked: root.approve() }
                TButton { text: Strings.t("Discard"); subtle: true; enabled: !!root.proposal; onClicked: root.discard() }
            }
            Text {
                text: root.status
                visible: text.length > 0
                color: Theme.textSecondary; font.pixelSize: Theme.fontSmall; wrapMode: Text.Wrap; Layout.fillWidth: true
            }
        }
    }
}
