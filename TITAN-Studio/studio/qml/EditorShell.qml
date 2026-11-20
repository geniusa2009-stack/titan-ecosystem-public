// TITAN Studio — editor shell: toolbar, workspace rail, workspaces, status bar.
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import TitanStudio

Item {
    id: shell
    objectName: "editorShell"
    property var workspaces: []
    signal viewportReady(var viewport)
    function toggleHistory() { history.opened ? history.close() : history.open() }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // ---- Toolbar ---------------------------------------------------------------
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 46
            color: Theme.toolbar
            Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.border }
            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 14
                anchors.rightMargin: 12
                spacing: 6
                Rectangle {
                    width: 22; height: 22; radius: 6; color: Theme.accent
                    Rectangle { x: 6; y: 6; width: 10; height: 2.5; radius: 1; color: "white" }
                    Rectangle { x: 9.75; y: 6; width: 2.5; height: 10; radius: 1; color: "white" }
                }
                ColumnLayout {
                    spacing: 0
                    Layout.leftMargin: 6
                    RowLayout {
                        spacing: 6
                        Text { text: engineClient.projectName; color: Theme.textPrimary; font.pixelSize: Theme.fontTitle; font.weight: Font.DemiBold }
                        Rectangle { width: 7; height: 7; radius: 3.5; color: Theme.warning; visible: engineClient.dirty; Accessible.name: Strings.t("Unsaved changes") }
                    }
                    Text { text: Strings.t(shell.workspaces[prefs.workspace] ? shell.workspaces[prefs.workspace].key : ""); color: Theme.textTertiary; font.pixelSize: Theme.fontCaption }
                }
                Item { Layout.preferredWidth: 18 }
                TIconButton { glyph: "↶"; tip: Strings.t("Undo") + (engineClient.undoLabel ? " — " + engineClient.undoLabel : "") + "  ⌘Z"; enabled: engineClient.canUndo; onClicked: engineClient.undo() }
                TIconButton { glyph: "↷"; tip: Strings.t("Redo") + (engineClient.redoLabel ? " — " + engineClient.redoLabel : "") + "  ⇧⌘Z"; enabled: engineClient.canRedo; onClicked: engineClient.redo() }
                TIconButton {
                    id: historyButton
                    objectName: "historyButton"
                    glyph: "☰"; tip: Strings.t("History") + "  ⌥⌘Z"
                    visible: engineClient.capabilities.indexOf("edit.history") >= 0
                    active: history.opened
                    onClicked: shell.toggleHistory()
                    HistoryPopup { id: history; y: historyButton.height + 4 }
                }
                Rectangle { width: 1; height: 20; color: Theme.border }
                TIconButton {
                    id: addButton
                    glyph: "+"; tip: Strings.t("Add") + "…"
                    onClicked: addMenu.popup(addButton, 0, addButton.height + 4)
                    Menu {
                        id: addMenu
                        MenuItem { text: "Cube"; onTriggered: engineClient.createPrimitive("cube") }
                        MenuItem { text: "Sphere"; onTriggered: engineClient.createPrimitive("sphere") }
                        MenuItem { text: "Cylinder"; onTriggered: engineClient.createPrimitive("cylinder") }
                        MenuItem { text: "Plane"; onTriggered: engineClient.createPrimitive("plane") }
                    }
                }
                Item { Layout.fillWidth: true }
                // Command palette affordance
                Rectangle {
                    Layout.preferredWidth: 300
                    Layout.preferredHeight: 28
                    radius: Theme.radius
                    color: Theme.field
                    border.color: paletteHover.hovered ? Theme.borderStrong : Theme.border
                    HoverHandler { id: paletteHover; cursorShape: Qt.PointingHandCursor }
                    TapHandler { onTapped: palette.openPalette() }
                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 10
                        anchors.rightMargin: 8
                        Glyph { text: "⌕"; size: Theme.px(13); color: Theme.textTertiary }
                        Text { text: Strings.t("Type a command…"); color: Theme.textTertiary; font.pixelSize: Theme.fontSmall; Layout.fillWidth: true }
                        Text { text: "⌘K"; color: Theme.textTertiary; font.pixelSize: Theme.fontCaption; font.family: Theme.monoFamily }
                    }
                }
                Item { Layout.fillWidth: true }
                TIconButton { glyph: Theme.dark ? "☀" : "☾"; tip: Strings.t("Toggle Light/Dark") + "  ⇧⌘L"; onClicked: win.toggleTheme() }
                TButton { text: Strings.t("Save"); primary: engineClient.dirty; enabled: engineClient.projectOpen; onClicked: engineClient.saveProject() }
            }
        }

        // ---- Crash recovery -------------------------------------------------------------
        Rectangle {
            objectName: "recoveryBanner"
            Layout.fillWidth: true
            visible: engineClient.projectOpen && !!engineClient.recovery.available
            Layout.preferredHeight: visible ? 40 : 0
            color: Qt.rgba(0.95, 0.65, 0.2, 0.14)
            border.color: Theme.border
            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 14; anchors.rightMargin: 10
                spacing: 10
                Text {
                    Layout.fillWidth: true
                    elide: Text.ElideRight
                    color: Theme.textPrimary; font.pixelSize: Theme.fontSmall
                    text: "This project has unsaved changes from a session that did not close cleanly (recovery point "
                          + (engineClient.recovery.savedAtMs ? new Date(engineClient.recovery.savedAtMs).toLocaleString() : "")
                          + ", " + (engineClient.recovery.objects || 0) + " objects)."
                }
                TButton { objectName: "recoveryRestore"; text: "Recover"; onClicked: engineClient.resolveRecovery(true) }
                TButton { objectName: "recoveryDiscard"; text: "Discard"; subtle: true; onClicked: engineClient.resolveRecovery(false) }
            }
        }
        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 0

            // ---- Workspace rail ----------------------------------------------------
            Rectangle {
                Layout.fillHeight: true
                Layout.preferredWidth: 56
                color: Theme.sidebar
                Rectangle { anchors.right: parent.right; width: 1; height: parent.height; color: Theme.border }
                ColumnLayout {
                    anchors.top: parent.top
                    anchors.horizontalCenter: parent.horizontalCenter
                    anchors.topMargin: 10
                    spacing: 4
                    Repeater {
                        model: shell.workspaces
                        delegate: TIconButton {
                            required property var modelData
                            required property int index
                            size: 40
                            glyph: modelData.glyph
                            tip: Strings.t(modelData.key) + "  ⌘" + (index + 1)
                            active: prefs.workspace === index
                            onClicked: prefs.workspace = index
                            opacity: index === 0 || index === 4 || index === 7 ? 1.0 : 0.72
                        }
                    }
                }
            }

            // ---- Workspaces -------------------------------------------------------
            StackLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                currentIndex: prefs.workspace

                // 0 World
                SplitView {
                    id: worldSplit
                    orientation: Qt.Horizontal
                    handle: Rectangle {
                        implicitWidth: 1
                        color: SplitHandle.pressed || SplitHandle.hovered ? Theme.accent : Theme.border
                        containmentMask: Item { x: -4; width: 9; height: worldSplit.height }
                    }
                    Hierarchy {
                        SplitView.preferredWidth: 260
                        SplitView.minimumWidth: 190
                    }
                    Rectangle {
                        SplitView.fillWidth: true
                        SplitView.minimumWidth: 320
                        color: Theme.canvasSurround
                        ViewportItem {
                            id: viewport
                            objectName: "viewport"
                            snapStep: prefs.snapStep
                            smoothFocus: !prefs.reducedMotion
                            anchors.fill: parent
                            engine: engineClient
                            scene: sceneModel
                            canvasColor: Theme.canvas
                            gridColor: Theme.grid
                            accentColor: Theme.accent
                            continuous: perfLive.checked
                            focus: true
                            Accessible.name: "3D viewport"
                            Component.onCompleted: shell.viewportReady(viewport)
                        }
                        // Floating controls: restrained translucency, only at the edges.
                        Rectangle {
                            anchors.top: parent.top; anchors.left: parent.left; anchors.margins: 10
                            height: 26; width: chip.implicitWidth + 20; radius: 13
                            color: Theme.overlay; border.color: Theme.border
                            Text { id: chip; anchors.centerIn: parent; text: "Perspective · Lit · " + viewport.backendName; color: Theme.textSecondary; font.pixelSize: Theme.fontCaption }
                        }
                        Row {
                            anchors.top: parent.top; anchors.right: parent.right; anchors.margins: 10
                            spacing: 4
                            TIconButton { glyph: "⊡"; tip: Strings.t("Frame Selection") + "  F"; enabled: engineClient.selection.length > 0; onClicked: win.frameSelection() }
                            TIconButton {
                                id: bookmarkButton
                                objectName: "bookmarksButton"
                                glyph: "☆"; tip: Strings.t("Camera Bookmarks") + "  ⌘B saves · ⌥1–9 recalls"
                                visible: win.canBookmark
                                active: bookmarks.opened
                                onClicked: bookmarks.opened ? bookmarks.close() : bookmarks.open()
                                Popup {
                                    id: bookmarks
                                    objectName: "bookmarksPopup"
                                    y: bookmarkButton.height + 4
                                    x: bookmarkButton.width - width
                                    width: 220
                                    padding: 6
                                    background: Rectangle { color: Theme.surface; border.color: Theme.border; radius: Theme.radiusLarge }
                                    contentItem: Column {
                                        spacing: 2
                                        TButton {
                                            objectName: "bookmarkSave"
                                            width: parent.width; subtle: true
                                            text: "Save current view"
                                            enabled: engineClient.cameraBookmarks.length < 32
                                            onClicked: win.saveBookmark()
                                        }
                                        Repeater {
                                            model: engineClient.cameraBookmarks
                                            delegate: Rectangle {
                                                required property int index
                                                required property var modelData
                                                width: parent ? parent.width : 0; height: Theme.rowHeight; radius: Theme.radiusSmall
                                                color: hov.hovered ? Theme.hover : "transparent"
                                                HoverHandler { id: hov }
                                                TapHandler { onTapped: { viewport.flyTo(modelData.camera); bookmarks.close() } }
                                                Text {
                                                    anchors.left: parent.left; anchors.leftMargin: 8; anchors.verticalCenter: parent.verticalCenter
                                                    text: (index < 9 ? "⌥" + (index + 1) + "  " : "") + modelData.name
                                                    color: Theme.textPrimary; font.pixelSize: Theme.fontSmall
                                                }
                                                Glyph {
                                                    anchors.right: parent.right; anchors.rightMargin: 8; anchors.verticalCenter: parent.verticalCenter
                                                    visible: hov.hovered; text: "×"; size: Theme.px(12)
                                                    Accessible.name: "Delete bookmark " + modelData.name
                                                    TapHandler { onTapped: win.deleteBookmark(index) }
                                                }
                                            }
                                        }
                                        Text {
                                            visible: engineClient.cameraBookmarks.length === 0
                                            text: "No saved views yet"; color: Theme.textTertiary; font.pixelSize: Theme.fontCaption
                                            leftPadding: 8
                                        }
                                    }
                                }
                            }
                            TIconButton { glyph: "⌂"; tip: Strings.t("Reset Camera") + "  Home"; onClicked: viewport.resetCamera() }
                        }
                        Rectangle {
                            anchors.bottom: parent.bottom; anchors.right: parent.right; anchors.margins: 10
                            height: 24; width: statsRow.implicitWidth + 16; radius: 12
                            color: Theme.overlay; border.color: viewport.continuous ? Theme.accent : Theme.border
                            Row {
                                id: statsRow
                                anchors.centerIn: parent
                                spacing: 8
                                Text {
                                    // Engine-rendered viewport: report the engine's measured rate, GPU time and
                                    // capture-to-screen latency, not Studio's re-presentation of the last frame.
                                    text: viewport.showingEngineFrames
                                          ? ("Engine " + (viewport.engineFps > 0 ? viewport.engineFps.toFixed(1) + " fps" : "on demand") + " · GPU " + (viewport.engineGpuMs > 0 ? viewport.engineGpuMs.toFixed(1) + " ms" : "n/a") + " · latency " + viewport.engineFrameLatencyMs.toFixed(0) + " ms")
                                          : viewport.continuous
                                          ? (viewport.frameMs.toFixed(2) + " ms · " + viewport.fps.toFixed(0) + " fps · GPU " + (viewport.gpuMs > 0 ? viewport.gpuMs.toFixed(2) + " ms" : "n/a") + " · " + viewport.drawCalls + " draws")
                                          : ("On-demand · " + viewport.drawCalls + " draws · CPU " + viewport.cpuRenderMs.toFixed(2) + " ms")
                                    color: Theme.textTertiary; font.pixelSize: Theme.fontCaption; font.family: Theme.monoFamily
                                }
                                Text {
                                    text: viewport.continuous ? "● LIVE" : "○ LIVE"
                                    color: viewport.continuous ? Theme.accentStrong : Theme.textTertiary
                                    font.pixelSize: Theme.fontCaption; font.weight: Font.DemiBold
                                    TapHandler { onTapped: perfLive.checked = !viewport.continuous }
                                    HoverHandler { cursorShape: Qt.PointingHandCursor }
                                }
                            }
                        }
                        Rectangle {
                            anchors.bottom: parent.bottom; anchors.left: parent.left; anchors.margins: 10
                            height: 24; width: hint.implicitWidth + 18; radius: 12
                            color: Theme.overlay; border.color: Theme.border
                            Text {
                                id: hint
                                anchors.centerIn: parent
                                text: viewport.previewCount > 0 ? ("World Builder preview · " + viewport.previewCount + " objects (not in scene until approved)")
                                                                : "Drag orbit · ⇧/right-drag pan · scroll zoom · click select · ⌥-drag move"
                                color: viewport.previewCount > 0 ? Theme.accentStrong : Theme.textTertiary
                                font.pixelSize: Theme.fontCaption
                            }
                        }
                    }
                    Rectangle {
                        SplitView.preferredWidth: 320
                        SplitView.minimumWidth: 260
                        color: Theme.surface
                        ColumnLayout {
                            anchors.fill: parent
                            spacing: 0
                            Segmented {
                                id: rightTabs
                                Layout.fillWidth: true
                                Layout.margins: 10
                                model: [Strings.t("Inspector"), Strings.t("World Builder")]
                            }
                            Rectangle { Layout.fillWidth: true; height: 1; color: Theme.border }
                            StackLayout {
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                currentIndex: rightTabs.currentIndex
                                Inspector {}
                                WorldBuilderPanel { viewport: viewport }
                            }
                        }
                    }
                }
                // 1 Code, 2 Materials, 3 Cinematic
                PlaceholderWorkspace { title: "Code"; glyph: "⟨⟩"; summary: "Source explorer, symbol navigation, diagnostics, build output and Git changes."; planned: ["Project source tree and editor integration", "Build output and compiler diagnostics", "Git status and diffs", "Engineering Agent suggestions (review-only)"] }
                PlaceholderWorkspace { title: "Materials"; glyph: "◐"; summary: "Material assets, shader graph and live preview."; planned: ["Material asset browser", "Node graph authoring", "Live preview and compile errors"] }
                PlaceholderWorkspace { title: "Cinematic"; glyph: "▷"; summary: "Timeline, cameras and sequencing."; planned: ["Timeline and tracks", "Camera tools", "Preview playback"] }
                // 4 Performance
                PerformanceWorkspace { viewport: viewport; liveSwitch: perfLive }
                // 5 AI Lab, 6 Source Engineering
                PlaceholderWorkspace { title: "AI Lab"; glyph: "✦"; summary: "Engineering Agent tasks, plans, diffs, tests and approvals; World Builder jobs."; planned: ["Agent runs in an isolated Git worktree", "Plan → diff → build → test → explicit approval", "World Builder recipe history (available today in the World workspace)"] }
                PlaceholderWorkspace { title: "Source Engineering"; glyph: "⌗"; summary: "Source inventory, licences, provenance and integration decisions."; planned: ["Source Vault registry viewer (see docs/SOURCE_INVENTORY.md today)", "Licence and rights matrix", "Dependency graphs and compatibility reports"] }
                // 7 Extensions
                ExtensionsWorkspace {}
                AssetsWorkspace {}
            }
        }

        // ---- Status bar -------------------------------------------------------------
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 26
            color: Theme.toolbar
            Rectangle { anchors.top: parent.top; width: parent.width; height: 1; color: Theme.border }
            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 12
                anchors.rightMargin: 12
                spacing: 14
                Rectangle { width: 7; height: 7; radius: 3.5; color: engineClient.connected ? Theme.success : Theme.error }
                Text { text: engineClient.connectionStatus + (engineClient.connected ? " · " + (engineClient.engineInfo.backend || "") : ""); color: Theme.textSecondary; font.pixelSize: Theme.fontCaption }
                Text { text: engineClient.dirty ? Strings.t("Unsaved changes") : Strings.t("Saved"); color: engineClient.dirty ? Theme.warning : Theme.textTertiary; font.pixelSize: Theme.fontCaption }
                Text {
                    property var sel: engineClient.selection
                    text: sel.length === 0 ? "" : sel.length === 1 ? ("Selected: " + (sceneModel.object(sel[0]).name || "")) : (sel.length + " selected")
                    color: Theme.textSecondary; font.pixelSize: Theme.fontCaption
                }
                Item { Layout.fillWidth: true }
                Text { text: sceneModel.objectCount + " objects · rev " + engineClient.revision; color: Theme.textTertiary; font.pixelSize: Theme.fontCaption; font.family: Theme.monoFamily }
                Text { text: "RTT " + engineClient.lastRttMs.toFixed(2) + " ms"; color: Theme.textTertiary; font.pixelSize: Theme.fontCaption; font.family: Theme.monoFamily }
                Text { text: engineClient.projectPath; color: Theme.textTertiary; font.pixelSize: Theme.fontCaption; elide: Text.ElideMiddle; Layout.maximumWidth: 360; LayoutMirroring.enabled: false }
            }
        }
    }
    // Shared "live rendering" switch state (owned here so the viewport can bind to it).
    QtObject { id: perfLive; property bool checked: false }
}
