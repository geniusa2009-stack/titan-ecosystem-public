// TITAN Studio — Inspector. Reads engine-owned properties from the scene mirror; every edit
// is sent to the engine as object.update (undoable), and the panel refreshes from events.
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import TitanStudio

Item {
    id: root
    property double objId: engineClient.selection.length > 0 ? engineClient.selection[0] : 0
    property var obj: ({})
    property string query: ""
    // Multi-object editing (engine capability edit.multi): every selected object is edited in one undoable
    // batch. Values the objects share are shown; differing ones show "Mixed". "Offset" mode adds to each
    // object's own value (scale multiplies) instead of assigning one value to all.
    readonly property var ids: engineClient.selection
    readonly property bool multi: ids.length > 1 && engineClient.capabilities.indexOf("edit.multi") >= 0
    property var objs: []
    property bool offsetMode: false
    readonly property bool anyLocked: multi ? objs.some(o => o.locked) : !!obj.locked  // locked objects cannot be moved

    function refresh() {
        obj = objId ? sceneModel.object(objId) : ({})
        // From the selection itself: onIdsChanged can run before the `multi` binding re-evaluates.
        objs = ids.length > 1 ? ids.map(id => sceneModel.object(id)).filter(o => o && o.name) : []
    }
    // Shared value per axis for a vector property (null where the objects differ).
    function shared(field, fallback) {
        if (!multi) return obj[field] || fallback
        const out = [0, 1, 2].map(i => {
            const first = (objs[0] && objs[0][field]) ? objs[0][field][i] : fallback[i]
            return objs.every(o => Math.abs(((o[field] || fallback)[i]) - first) < 1e-6) ? first : null
        })
        return out
    }
    function vecValue(field, fallback) {
        if (multi && offsetMode) return field === "scale" ? [1, 1, 1] : [0, 0, 0]
        return shared(field, fallback).map((v, i) => v === null ? fallback[i] : v)
    }
    function vecMixed(field, fallback) {
        if (!multi || offsetMode) return [false, false, false]
        return shared(field, fallback).map(v => v === null)
    }
    function setVecMany(field, axis, v, key) {
        const comp = [null, null, null]
        comp[axis] = v
        const p = { ids: ids, mode: offsetMode ? "offset" : "set", coalesce: key }
        p[field] = comp
        engineClient.request("object.transformMany", p, (r, err) => { if (err) engineClient.notify(err.message, "error") })
    }
    function setShared(props) {
        engineClient.request("object.updateMany", { ids: ids, set: props }, (r, err) => { if (err) engineClient.notify(err.message, "error") })
    }
    onIdsChanged: refresh()
    function matches(label) { return query.length === 0 || label.toLowerCase().indexOf(query.toLowerCase()) >= 0 }
    function setVec(field, axis, v, key) {
        if (multi) return setVecMany(field, axis, v, key)
        const cur = (obj[field] || [0, 0, 0]).slice()
        cur[axis] = v
        const t = {}
        t[field] = cur
        engineClient.updateObject(objId, { transform: t }, key)
    }
    onObjIdChanged: refresh()
    Connections { target: sceneModel; function onSceneDataChanged() { root.refresh() } }

    // Empty state
    ColumnLayout {
        anchors.centerIn: parent
        visible: !root.objId || !root.obj.name
        spacing: 6
        width: parent.width - 40
        Text { text: Strings.t("No selection"); color: Theme.textSecondary; font.pixelSize: Theme.fontTitle; Layout.alignment: Qt.AlignHCenter }
        Text { text: Strings.t("Select an object in the viewport or the scene list."); color: Theme.textTertiary; font.pixelSize: Theme.fontSmall; wrapMode: Text.Wrap; horizontalAlignment: Text.AlignHCenter; Layout.fillWidth: true }
    }

    Flickable {
        anchors.fill: parent
        visible: !!root.objId && !!root.obj.name
        contentHeight: col.implicitHeight + 24
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ScrollBar {}

        ColumnLayout {
            id: col
            x: 14
            y: 12
            width: parent.width - 28
            spacing: 14

            TTextField {
                Layout.fillWidth: true
                placeholderText: Strings.t("Search") + " " + Strings.t("Inspector").toLowerCase()
                onTextChanged: root.query = text
                Accessible.name: "Search properties"
            }

            // Identity
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 6
                visible: root.matches("name") || root.matches("type") || root.matches("id")
                SectionHeader {
                    title: root.multi ? root.ids.length + " objects" : (root.obj.type || "")
                    detail: root.multi ? "edited together · one undo step" : "ID " + Math.round(root.objId)
                    Layout.fillWidth: true
                }
                TTextField {
                    objectName: "inspectorName"
                    visible: !root.multi
                    Layout.fillWidth: true
                    text: root.obj.name || ""
                    font.pixelSize: Theme.fontTitle
                    font.weight: Font.DemiBold
                    Accessible.name: Strings.t("Name")
                    onEditingFinished: if (text.length && text !== root.obj.name) engineClient.updateObject(root.objId, { name: text })
                }
                Text {
                    visible: !!root.obj.recipeId
                    text: "Generated by World Builder recipe " + root.obj.recipeId
                    color: Theme.accentStrong; font.pixelSize: Theme.fontSmall
                }
                Text {
                    visible: !!root.obj.parentName
                    text: "Parent: " + root.obj.parentName
                    color: Theme.textTertiary; font.pixelSize: Theme.fontSmall
                }
                Text {
                    visible: root.multi
                    text: root.objs.map(o => o.name).join(", ")
                    color: Theme.textSecondary; font.pixelSize: Theme.fontSmall; elide: Text.ElideRight; Layout.fillWidth: true
                }
                Text {
                    visible: engineClient.selection.length > 1 && !root.multi
                    text: engineClient.selection.length + " objects selected — this engine edits one object at a time (showing the first)"
                    color: Theme.textTertiary; font.pixelSize: Theme.fontSmall; wrapMode: Text.Wrap; Layout.fillWidth: true
                }
            }

            Rectangle { Layout.fillWidth: true; height: 1; color: Theme.border; visible: transformSection.visible }

            ColumnLayout {
                id: transformSection
                Layout.fillWidth: true
                spacing: 10
                visible: root.matches("transform") || root.matches("position") || root.matches("rotation") || root.matches("scale")
                SectionHeader { title: Strings.t("Transform"); detail: root.anyLocked ? "locked" : root.multi && root.offsetMode ? "offset · add (scale ×)" : "local · m / °"; Layout.fillWidth: true }
                Segmented {
                    objectName: "inspectorMode"
                    visible: root.multi
                    model: ["Set", "Offset"]
                    currentIndex: root.offsetMode ? 1 : 0
                    onActivated: (i) => root.offsetMode = i === 1
                }
                Vec3Editor {
                    Layout.fillWidth: true
                    label: Strings.t("Position")
                    enabled: !root.anyLocked
                    fieldPrefix: "inspectorPos"
                    value: root.vecValue("position", [0, 0, 0])
                    mixed: root.vecMixed("position", [0, 0, 0])
                    visible: root.matches("position") || root.matches("transform")
                    onEdited: (axis, v, key) => root.setVec("position", axis, v, key)
                }
                Vec3Editor {
                    Layout.fillWidth: true
                    label: Strings.t("Rotation")
                    enabled: !root.anyLocked
                    fieldPrefix: "inspectorRot"
                    step: 1
                    decimals: 2
                    value: root.vecValue("rotation", [0, 0, 0])
                    mixed: root.vecMixed("rotation", [0, 0, 0])
                    visible: root.matches("rotation") || root.matches("transform")
                    onEdited: (axis, v, key) => root.setVec("rotation", axis, v, key)
                }
                Vec3Editor {
                    Layout.fillWidth: true
                    label: Strings.t("Scale")
                    enabled: !root.anyLocked
                    fieldPrefix: "inspectorScale"
                    step: 0.05
                    value: root.vecValue("scale", [1, 1, 1])
                    mixed: root.vecMixed("scale", [1, 1, 1])
                    visible: root.matches("scale") || root.matches("transform")
                    onEdited: (axis, v, key) => { if (Math.abs(v) > 1e-6) root.setVec("scale", axis, v, key); else engineClient.notify("Scale cannot be zero", "error") }
                }
            }

            Rectangle { Layout.fillWidth: true; height: 1; color: Theme.border; visible: appearance.visible }

            ColumnLayout {
                id: appearance
                Layout.fillWidth: true
                spacing: 10
                visible: root.obj.type !== "group" && (root.matches("appearance") || root.matches("color") || root.matches("visible"))
                SectionHeader { title: Strings.t("Appearance "); Layout.fillWidth: true }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8
                    Text { text: Strings.t("Color"); color: Theme.textSecondary; font.pixelSize: Theme.fontSmall; Layout.preferredWidth: 60 }
                    Rectangle {
                        width: 22; height: 22; radius: 5
                        color: root.obj.color ? Qt.rgba(root.obj.color[0], root.obj.color[1], root.obj.color[2], 1) : "gray"
                        border.color: Theme.borderStrong
                    }
                    Repeater {
                        model: [[0.44, 0.6, 0.84], [0.86, 0.52, 0.38], [0.48, 0.72, 0.52], [0.9, 0.78, 0.42], [0.78, 0.78, 0.74], [0.24, 0.26, 0.3]]
                        delegate: Rectangle {
                            required property var modelData
                            width: 16; height: 16; radius: 8
                            color: Qt.rgba(modelData[0], modelData[1], modelData[2], 1)
                            border.color: Theme.border
                            TapHandler { onTapped: root.multi ? root.setShared({ color: parent.modelData }) : engineClient.updateObject(root.objId, { color: parent.modelData }) }
                            HoverHandler { cursorShape: Qt.PointingHandCursor }
                        }
                    }
                    Item { Layout.fillWidth: true }
                }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 6
                    visible: !root.multi  // per-channel values are per object; swatches apply to all
                    Repeater {
                        model: 3
                        delegate: NumberField {
                            required property int index
                            Layout.fillWidth: true
                            value: root.obj.color ? root.obj.color[index] : 0
                            step: 0.02
                            minimum: 0
                            maximum: 1
                            Accessible.name: "Color " + ["red", "green", "blue"][index]
                            onCommitted: (v, key) => {
                                const c = (root.obj.color || [0, 0, 0]).slice()
                                c[index] = v
                                engineClient.updateObject(root.objId, { color: c }, key)
                            }
                        }
                    }
                }
                RowLayout {
                    Layout.fillWidth: true
                    Text { text: Strings.t("Visible"); color: Theme.textSecondary; font.pixelSize: Theme.fontSmall; Layout.fillWidth: true }
                    Switch {
                        id: visSwitch
                        objectName: "inspectorVisible"
                        checked: root.multi ? root.objs.every(o => o.visible !== false) : root.obj.visible !== false
                        onToggled: root.multi ? root.setShared({ visible: checked }) : engineClient.updateObject(root.objId, { visible: checked })
                        indicator: Rectangle {
                            implicitWidth: 34; implicitHeight: 20; radius: 10
                            color: visSwitch.checked ? Theme.accent : Theme.surfaceSecondary
                            border.color: Theme.border
                            Rectangle { x: visSwitch.checked ? parent.width - width - 2 : 2; y: 2; width: 16; height: 16; radius: 8; color: "white"
                                Behavior on x { NumberAnimation { duration: Theme.durFast } } }
                        }
                    }
                }
            }

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 8
                visible: !root.multi && root.obj.type === "light" && (root.matches("light") || root.matches("intensity"))
                Rectangle { Layout.fillWidth: true; height: 1; color: Theme.border }
                SectionHeader { title: Strings.t("Light "); Layout.fillWidth: true }
                RowLayout {
                    Layout.fillWidth: true
                    Text { text: Strings.t("Intensity"); color: Theme.textSecondary; font.pixelSize: Theme.fontSmall; Layout.preferredWidth: 60 }
                    NumberField {
                        Layout.fillWidth: true
                        value: root.obj.intensity || 0
                        step: 0.05
                        minimum: 0
                        maximum: 20
                        onCommitted: (v, key) => engineClient.updateObject(root.objId, { intensity: v }, key)
                    }
                }
            }
        }
    }
}
