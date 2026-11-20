// TITAN Studio — scene hierarchy panel.
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import TitanStudio

Rectangle {
    id: root
    objectName: "hierarchy"
    color: Theme.sidebar
    readonly property bool canReparent: win.canEdit("edit.reparent")
    readonly property bool canLock: win.canEdit("edit.lock")
    readonly property bool canMulti: win.canEdit("edit.multi")
    readonly property var typeKeys: ["", "mesh", "light", "camera", "group"]
    // Filtering never changes the selection; selected objects the filter hides are counted instead.
    property int hiddenSelected: 0
    function countHidden() {
        hiddenSelected = engineClient.selection.filter(id => sceneModel.rowForId(id) < 0).length
    }
    Connections {
        target: sceneModel
        function onModelReset() { root.countHidden() }
    }
    Connections {
        target: engineClient
        function onSelectionChanged() { root.countHidden() }
    }
    // One undo step for the whole selection when the engine supports batches.
    function setOnSelection(props) {
        const ids = engineClient.selection
        if (!ids.length) return
        if (ids.length === 1) engineClient.updateObject(ids[0], props)
        else engineClient.request("object.updateMany", { ids: ids, set: props }, (r, err) => { if (err) engineClient.notify(err.message, "error") })
    }
    // Drag-and-drop parenting: dragging a selected row moves the whole selection; dropping on a row parents
    // under it, on empty space moves to the scene root. World transforms are kept unless Alt is held.
    property var dragIds: []
    property bool dragKeepWorld: true
    property double dropTarget: -1  // row under the pointer while dragging; 0 = scene root, -1 = none
    function beginDrag(id, keepWorld) {
        const sel = engineClient.selection
        dragIds = sel.indexOf(id) >= 0 ? sel.slice() : [id]
        dragKeepWorld = keepWorld
    }
    function reparent(ids, parent, keepWorld) {
        if (!ids.length || ids.indexOf(parent) >= 0) return
        engineClient.request("object.reparent", { ids: ids, parent: parent, keepWorld: keepWorld }, (r, err) => {
            if (err) engineClient.notify(err.message, "error")
        })
    }
    // Hit-tests the rows: `p` is in list coordinates. Outside the list -> -1 (cancel).
    function targetAt(p) {
        if (p.x < 0 || p.y < 0 || p.x > list.width || p.y > list.height) return -1
        const it = list.itemAt(p.x + list.contentX, p.y + list.contentY)
        return it ? it.oid : 0
    }
    function endDrag() {
        const ids = dragIds, target = dropTarget
        dragIds = []
        dropTarget = -1
        if (target >= 0) reparent(ids, target, dragKeepWorld)
    }
    Rectangle {
        id: dragProxy
        z: 10
        visible: root.dragIds.length > 0
        width: proxyText.implicitWidth + 16; height: Theme.rowHeight; radius: Theme.radiusSmall
        color: Theme.selection
        Text {
            id: proxyText; anchors.centerIn: parent
            text: root.dragIds.length === 1 ? (sceneModel.object(root.dragIds[0]).name || "") : root.dragIds.length + " objects"
            color: Theme.selectionText; font.pixelSize: Theme.fontSmall
        }
    }
    function glyphFor(type, shape) {
        if (type === "light") return "✺"
        if (type === "camera") return "◫"
        if (type === "group") return "▤"
        return ({ cube: "◼", sphere: "●", cylinder: "▮", plane: "▭" })[shape] || "◇"
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 10
        spacing: 8
        SectionHeader { title: Strings.t("Scene"); detail: sceneModel.objectCount + ""; Layout.fillWidth: true }
        TTextField {
            id: search
            objectName: "hierarchySearch"
            Layout.fillWidth: true
            placeholderText: Strings.t("Search")
            onTextChanged: searchDelay.restart()  // the model rebuilds once typing pauses, not per keystroke
            Keys.onReturnPressed: { searchDelay.stop(); sceneModel.filter = text }
            Accessible.name: "Filter scene objects"
            Timer { id: searchDelay; interval: 150; onTriggered: sceneModel.filter = search.text }
        }
        Segmented {
            objectName: "hierarchyType"
            Layout.fillWidth: true
            model: ["All", "◼", "✺", "◫", "▤"]
            currentIndex: Math.max(0, root.typeKeys.indexOf(sceneModel.typeFilter))
            onActivated: (i) => sceneModel.typeFilter = root.typeKeys[i]
            Accessible.name: "Filter by type: all, meshes, lights, cameras, groups"
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: 4
            visible: engineClient.selection.length > 0 || sceneModel.filter.length > 0 || sceneModel.typeFilter.length > 0
            Text {
                objectName: "hierarchySelectionInfo"
                text: engineClient.selection.length + " selected" + (root.hiddenSelected ? " · " + root.hiddenSelected + " hidden by filter" : "")
                color: Theme.textTertiary; font.pixelSize: Theme.fontCaption
                Layout.fillWidth: true; elide: Text.ElideRight
            }
            TButton {
                objectName: "hierarchySelectMatches"
                visible: sceneModel.filter.length > 0 || sceneModel.typeFilter.length > 0
                text: "Select all"; subtle: true
                onClicked: engineClient.select(sceneModel.matchingIds())
            }
            TButton {
                objectName: "hierarchyHideSelected"
                visible: engineClient.selection.length > 1 && root.canMulti
                text: "Hide"; subtle: true
                onClicked: root.setOnSelection({ visible: false })
            }
            TButton {
                visible: engineClient.selection.length > 1 && root.canMulti
                text: "Show"; subtle: true
                onClicked: root.setOnSelection({ visible: true })
            }
            TButton {
                objectName: "hierarchyLockSelected"
                visible: engineClient.selection.length > 1 && root.canMulti && root.canLock
                text: "Lock"; subtle: true
                onClicked: root.setOnSelection({ locked: true })
            }
            TButton {
                visible: engineClient.selection.length > 1 && root.canMulti && root.canLock
                text: "Unlock"; subtle: true
                onClicked: root.setOnSelection({ locked: false })
            }
        }
        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true
        Rectangle {  // dropping below the rows moves to the scene root
            anchors.fill: parent; z: 1
            color: "transparent"; border.color: Theme.accent; radius: Theme.radiusSmall
            visible: root.dragIds.length > 0 && root.dropTarget === 0
        }
        ListView {
            id: list
            anchors.fill: parent
            clip: true
            model: sceneModel
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
            keyNavigationEnabled: true
            activeFocusOnTab: true
            Keys.onUpPressed: moveSelection(-1)
            Keys.onDownPressed: moveSelection(1)
            Keys.onDeletePressed: engineClient.deleteSelection()
            function moveSelection(d) {
                const sel = engineClient.selection
                let row = sel.length ? sceneModel.rowForId(sel[0]) : -1
                row = Math.max(0, Math.min(count - 1, row + d))
                currentIndex = row
                const item = itemAtIndex(row)
                if (item) engineClient.select([item.oid])
            }
            delegate: Rectangle {
                id: row
                required property int index
                required property double objectId
                required property string name
                required property string type
                required property string shape
                required property int depth
                required property bool hasChildren
                required property bool expanded
                required property bool selected
                required property bool objectVisible
                required property bool objectLocked
                required property bool generated
                readonly property double oid: objectId
                objectName: "hierarchyRow:" + name
                width: list.width
                height: Theme.rowHeight
                radius: Theme.radiusSmall
                color: selected ? Theme.selection : hover.hovered ? Theme.hover : "transparent"
                border.color: root.dragIds.length > 0 && root.dropTarget === row.objectId ? Theme.accent : "transparent"
                HoverHandler { id: hover }
                function dragActive(active, keepWorld) {
                    if (active) root.beginDrag(row.objectId, keepWorld)
                    else if (root.dragIds.length) root.endDrag()
                }
                function dragMoved(pos) {
                    const p = row.mapToItem(root, pos.x, pos.y)
                    dragProxy.x = p.x + 6; dragProxy.y = p.y - 8
                    root.dropTarget = root.targetAt(row.mapToItem(list, pos.x, pos.y))
                }
                DragHandler {  // keeps world transforms
                    target: null; enabled: root.canReparent; acceptedModifiers: Qt.NoModifier
                    onActiveChanged: row.dragActive(active, true)
                    onCentroidChanged: if (active) row.dragMoved(centroid.position)
                }
                DragHandler {  // Alt: keeps local transforms
                    target: null; enabled: root.canReparent; acceptedModifiers: Qt.AltModifier
                    onActiveChanged: row.dragActive(active, false)
                    onCentroidChanged: if (active) row.dragMoved(centroid.position)
                }
                TapHandler {
                    onTapped: (ev) => {
                        list.forceActiveFocus()
                        if (ev.modifiers & (Qt.ControlModifier | Qt.MetaModifier | Qt.ShiftModifier)) {
                            const s = engineClient.selection.slice()
                            const i = s.indexOf(row.objectId)
                            if (i >= 0) s.splice(i, 1); else s.push(row.objectId)
                            engineClient.select(s)
                        } else engineClient.select([row.objectId])
                    }
                    onDoubleTapped: if (win.viewport) { engineClient.select([row.objectId]); win.frameSelection() }
                }
                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 4 + row.depth * 14
                    anchors.rightMargin: 4
                    spacing: 4
                    Glyph {
                        text: row.hasChildren ? (row.expanded ? "▾" : "▸") : ""
                        size: Theme.px(11)
                        Layout.preferredWidth: 12
                        TapHandler { onTapped: sceneModel.toggleExpanded(row.index) }
                    }
                    Glyph { objectName: "hierarchyGlyph:" + row.name; text: root.glyphFor(row.type, row.shape); size: Theme.px(11); color: row.selected ? Theme.accentStrong : Theme.textTertiary; Layout.preferredWidth: 14 }
                    Text {
                        text: row.name
                        color: !row.objectVisible ? Theme.textDisabled : row.selected ? Theme.selectionText : Theme.textPrimary
                        font.pixelSize: Theme.fontBody
                        elide: Text.ElideRight
                        Layout.fillWidth: true
                    }
                    Rectangle {
                        visible: row.generated && row.type === "group"
                        implicitWidth: wb.implicitWidth + 8; implicitHeight: 15; radius: 3
                        color: Theme.selection
                        Text { id: wb; anchors.centerIn: parent; text: "WB"; font.pixelSize: Theme.fontCaption; font.weight: Font.DemiBold; color: Theme.accentStrong }
                    }
                    Glyph {
                        objectName: "hierarchyLock:" + row.name
                        visible: root.canLock && (hover.hovered || row.objectLocked)
                        text: row.objectLocked ? "⊠" : "⊡"
                        size: Theme.px(11)
                        color: row.objectLocked ? Theme.accentStrong : Theme.textTertiary
                        Accessible.name: row.objectLocked ? "Unlock" : "Lock"
                        TapHandler { onTapped: engineClient.updateObject(row.objectId, { locked: !row.objectLocked }) }
                    }
                    Glyph {
                        visible: hover.hovered || !row.objectVisible
                        text: row.objectVisible ? "◉" : "○"
                        size: Theme.px(11)
                        Accessible.name: row.objectVisible ? "Hide" : "Show"
                        TapHandler { onTapped: engineClient.updateObject(row.objectId, { visible: !row.objectVisible }) }
                    }
                }
            }
            Text {
                anchors.centerIn: parent
                visible: list.count === 0
                text: sceneModel.filter.length ? "No matches" : ""
                color: Theme.textTertiary
                font.pixelSize: Theme.fontSmall
            }
        }
        }
    }
}
