// TITAN Studio — main window. Original TITAN design.
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Dialogs
import QtQuick.Layouts
import TitanStudio

ApplicationWindow {
    id: win
    width: 1440
    height: 900
    minimumWidth: 980
    minimumHeight: 620
    visible: true
    color: Theme.background
    title: engineClient.projectOpen
           ? engineClient.projectName + (engineClient.dirty ? " — " + Strings.t("Unsaved changes") : "") + "  ·  TITAN Studio"
           : "TITAN Studio"

    LayoutMirroring.enabled: Strings.rtl
    LayoutMirroring.childrenInherit: true
    font.pixelSize: Theme.fontBody

    Binding { target: Theme; property: "dark"; value: prefs.themeMode === "dark" || (prefs.themeMode === "system" && Qt.styleHints.colorScheme === Qt.ColorScheme.Dark) }
    Binding { target: Theme; property: "textScale"; value: prefs.textScale }
    Binding { target: Theme; property: "reducedMotion"; value: prefs.reducedMotion }
    Binding { target: Strings; property: "lang"; value: prefs.language }

    // Scene editing through the engine (one undo step each); the engine selects the result.
    readonly property bool canClipboard: win.canEdit("edit.clipboard") && !win.textEditing
    // Text fields keep ⌘C/⌘V for text: object clipboard shortcuts are off while one has focus.
    readonly property bool textEditing: !!win.activeFocusItem && win.activeFocusItem.selectedText !== undefined
    readonly property bool canBookmark: engineClient.projectOpen && engineClient.capabilities.indexOf("camera.bookmarks") >= 0 && !!win.viewport
    function frameSelection() {
        if (win.viewport && !win.viewport.frameSelection()) engineClient.notify("Nothing to frame: select an object first")
    }
    function saveBookmark() {
        const list = engineClient.cameraBookmarks.slice()
        let n = list.length + 1
        while (list.some(b => b.name === "View " + n)) ++n
        list.push({ name: "View " + n, camera: win.viewport.cameraState() })
        engineClient.setCameraBookmarks(list)
        engineClient.notify("Saved camera bookmark View " + n + (list.length <= 9 ? " (⌥" + list.length + ")" : ""))
    }
    function recallBookmark(i) {
        const b = engineClient.cameraBookmarks[i]
        if (b) win.viewport.flyTo(b.camera)
    }
    function deleteBookmark(i) {
        const list = engineClient.cameraBookmarks.slice()
        list.splice(i, 1)
        engineClient.setCameraBookmarks(list)
    }
    function canEdit(capability) { return engineClient.projectOpen && engineClient.capabilities.indexOf(capability) >= 0 }
    function editSelection(method) {
        const p = method === "object.ungroup" ? { id: engineClient.selection[0] } : { ids: engineClient.selection }
        engineClient.request(method, p, (r, err) => { if (err) engineClient.notify(err.message, "error") })
    }
    readonly property var workspaces: [
        { key: "World", glyph: "◍" }, { key: "Code", glyph: "⟨⟩" }, { key: "Materials", glyph: "◐" },
        { key: "Cinematic", glyph: "▷" }, { key: "Performance", glyph: "∿" }, { key: "AI Lab", glyph: "✦" },
        { key: "Source Engineering", glyph: "⌗" }, { key: "Extensions", glyph: "⬡" }, { key: "Assets", glyph: "▦" }
    ]

    function urlToPath(u) {
        let s = u.toString()
        if (s.startsWith("file://")) s = s.substring(7)
        return decodeURIComponent(s)
    }
    function toggleTheme() { prefs.themeMode = Theme.dark ? "light" : "dark" }
    property var viewport: null  // set by EditorShell
    property bool closeConfirmed: false

    // Command registry: the palette, menus and shortcuts all run the same commands.
    readonly property var commands: [
        { title: "Save", shortcut: "⌘S", enabled: () => engineClient.projectOpen, run: () => engineClient.saveProject() },
        { title: "Undo", shortcut: "⌘Z", enabled: () => engineClient.canUndo, run: () => engineClient.undo() },
        { title: "Redo", shortcut: "⇧⌘Z", enabled: () => engineClient.canRedo, run: () => engineClient.redo() },
        { title: "New Project…", shortcut: "⌘N", enabled: () => engineClient.connected, run: () => newDialog.open() },
        { title: "Open Project…", shortcut: "⌘O", enabled: () => engineClient.connected, run: () => openDialog.open() },
        { title: "Close Project", shortcut: "", enabled: () => engineClient.projectOpen, run: () => requestClose(false) },
        { title: "Add Cube", shortcut: "", enabled: () => engineClient.projectOpen, run: () => engineClient.createPrimitive("cube") },
        { title: "Add Sphere", shortcut: "", enabled: () => engineClient.projectOpen, run: () => engineClient.createPrimitive("sphere") },
        { title: "Add Cylinder", shortcut: "", enabled: () => engineClient.projectOpen, run: () => engineClient.createPrimitive("cylinder") },
        { title: "Add Plane", shortcut: "", enabled: () => engineClient.projectOpen, run: () => engineClient.createPrimitive("plane") },
        { title: "Duplicate", shortcut: "⌘D", enabled: () => win.canEdit("edit.duplicate") && engineClient.selection.length > 0, run: () => win.editSelection("object.duplicate") },
        { title: "Group", shortcut: "⌘G", enabled: () => win.canEdit("edit.group") && engineClient.selection.length > 0, run: () => win.editSelection("object.group") },
        { title: "Ungroup", shortcut: "⇧⌘G", enabled: () => win.canEdit("edit.group") && engineClient.selection.length === 1, run: () => win.editSelection("object.ungroup") },
        { title: prefs.snapStep > 0 ? "Grid Snap: Off" : "Grid Snap: On (0.5 m)", shortcut: "", enabled: () => true, run: () => prefs.snapStep = prefs.snapStep > 0 ? 0 : 0.5 },
        { title: "Save Scene as Template", shortcut: "", enabled: () => win.canEdit("templates"),
          run: () => engineClient.request("project.saveTemplate", { name: engineClient.projectName }, (r, err) => engineClient.notify(err ? "Template not saved: " + err.message : "Saved template \u201c" + r.name + "\u201d (" + r.objects + " objects)", err ? "error" : "info")) },
        { title: "Create Prefab from Selection", shortcut: "", enabled: () => engineClient.selection.length === 1 && engineClient.capabilities.indexOf("prefabs") >= 0,
          run: () => engineClient.request("prefab.create", { id: engineClient.selection[0] }, (r, err) => engineClient.notify(err ? "Prefab not created: " + err.message : "Saved prefab " + r.path + " (" + r.objects + " objects)", err ? "error" : "info")) },
        { title: "Delete", shortcut: "⌫", enabled: () => engineClient.selection.length > 0, run: () => engineClient.deleteSelection() },
        { title: "Frame Selection", shortcut: "F", enabled: () => !!win.viewport && engineClient.selection.length > 0, run: () => win.frameSelection() },
        { title: "Copy", shortcut: "⌘C", enabled: () => win.canEdit("edit.clipboard") && engineClient.selection.length > 0, run: () => engineClient.copySelection(false) },
        { title: "Cut", shortcut: "⌘X", enabled: () => win.canEdit("edit.clipboard") && engineClient.selection.length > 0, run: () => engineClient.copySelection(true) },
        { title: "Paste", shortcut: "⌘V", enabled: () => win.canEdit("edit.clipboard"), run: () => engineClient.pasteClipboard(false) },
        { title: "Paste Into Selection", shortcut: "⇧⌘V", enabled: () => win.canEdit("edit.clipboard") && engineClient.selection.length === 1, run: () => engineClient.pasteClipboard(true) },
        { title: "Save Camera Bookmark", shortcut: "⌘B", enabled: () => win.canBookmark && engineClient.cameraBookmarks.length < 32, run: () => win.saveBookmark() },
        { title: "Reset Camera", shortcut: "Home", enabled: () => !!win.viewport, run: () => win.viewport.resetCamera() },
        { title: "Toggle Light/Dark", shortcut: "⇧⌘L", enabled: () => true, run: () => toggleTheme() },
        { title: "Restart Engine", shortcut: "", enabled: () => true, run: () => engineClient.restart() },
        { title: "Show Unreal Editor Window", shortcut: "", enabled: () => engineClient.connected && engineClient.engineInfo.backend === "unreal-editor", run: () => engineClient.request("unreal.window", { visible: true }) },
        { title: "Hide Unreal Editor Window", shortcut: "", enabled: () => engineClient.connected && engineClient.engineInfo.backend === "unreal-editor", run: () => engineClient.request("unreal.window", { visible: false }) },
        { title: "Engine: TITAN Reference", shortcut: "", enabled: () => engineClient.engineKind !== "reference", run: () => { prefs.engineKind = "reference"; engineClient.engineKind = "reference"; engineClient.restart() } },
        { title: "Engine: Unreal Engine 5.8.4", shortcut: "", enabled: () => engineClient.engineKind !== "unreal" && engineClient.unrealAvailable, run: () => { prefs.engineKind = "unreal"; engineClient.engineKind = "unreal"; engineClient.restart() } },
        { title: "Language: English", shortcut: "", enabled: () => prefs.language !== "en", run: () => prefs.language = "en" },
        { title: "Language: العربية", shortcut: "", enabled: () => prefs.language !== "ar", run: () => prefs.language = "ar" },
        { title: "Larger Text", shortcut: "⌘+", enabled: () => prefs.textScale < 1.4, run: () => prefs.textScale = prefs.textScale + 0.1 },
        { title: "Smaller Text", shortcut: "⌘−", enabled: () => prefs.textScale > 0.85, run: () => prefs.textScale = prefs.textScale - 0.1 },
        { title: "Reduce Motion: Toggle", shortcut: "", enabled: () => true, run: () => prefs.reducedMotion = !prefs.reducedMotion }
    ].concat(workspaces.map((w, i) => ({ title: "Go to " + w.key, shortcut: "⌘" + (i + 1), enabled: () => engineClient.projectOpen, run: () => prefs.workspace = i })))

    function requestClose(quitAfter) {
        if (engineClient.dirty) { unsavedDialog.quitAfter = quitAfter; unsavedDialog.open(); return false }
        if (quitAfter) { closeConfirmed = true; Qt.quit() } else engineClient.closeProject(false)
        return true
    }
    onClosing: (close) => {
        if (closeConfirmed || !engineClient.dirty) return
        close.accepted = false
        requestClose(true)
    }

    Connections {
        target: engineClient
        function onProjectLoaded() { prefs.addRecent(engineClient.projectPath, engineClient.projectName) }
    }

    StackLayout {
        anchors.fill: parent
        currentIndex: engineClient.projectOpen ? 1 : 0
        Launcher { onNewProject: newDialog.open(); onOpenProject: openDialog.open() }
        EditorShell { id: shell; workspaces: win.workspaces; onViewportReady: (v) => win.viewport = v }
    }

    // Shortcuts (Ctrl maps to ⌘ on macOS)
    Shortcut { sequences: [StandardKey.Save]; enabled: engineClient.projectOpen; onActivated: engineClient.saveProject() }
    Shortcut { sequences: [StandardKey.Undo]; enabled: engineClient.canUndo; onActivated: engineClient.undo() }
    Shortcut { sequences: [StandardKey.Redo, "Ctrl+Shift+Z"]; enabled: engineClient.canRedo; onActivated: engineClient.redo() }
    Shortcut { sequences: [StandardKey.Open]; onActivated: openDialog.open() }
    Shortcut { sequences: [StandardKey.New]; onActivated: newDialog.open() }
    Shortcut { sequence: "Ctrl+K"; onActivated: palette.openPalette() }
    Shortcut { sequence: "Ctrl+D"; enabled: win.canEdit("edit.duplicate") && engineClient.selection.length > 0; onActivated: win.editSelection("object.duplicate") }
    Shortcut { sequence: "Ctrl+G"; enabled: win.canEdit("edit.group") && engineClient.selection.length > 0; onActivated: win.editSelection("object.group") }
    Shortcut { sequence: "Ctrl+Shift+G"; enabled: win.canEdit("edit.group") && engineClient.selection.length === 1; onActivated: win.editSelection("object.ungroup") }
    Shortcut { sequence: "Ctrl+Shift+P"; onActivated: palette.openPalette() }
    Shortcut { sequences: [StandardKey.Copy]; enabled: win.canClipboard && engineClient.selection.length > 0; onActivated: engineClient.copySelection(false) }
    Shortcut { sequences: [StandardKey.Cut]; enabled: win.canClipboard && engineClient.selection.length > 0; onActivated: engineClient.copySelection(true) }
    Shortcut { sequences: [StandardKey.Paste]; enabled: win.canClipboard; onActivated: engineClient.pasteClipboard(false) }
    Shortcut { sequence: "Ctrl+Shift+V"; enabled: win.canClipboard && engineClient.selection.length === 1; onActivated: engineClient.pasteClipboard(true) }
    Shortcut { sequence: "Ctrl+Alt+Z"; enabled: win.canEdit("edit.history"); onActivated: shell.toggleHistory() }
    Shortcut { sequence: "Ctrl+B"; enabled: win.canBookmark && engineClient.cameraBookmarks.length < 32; onActivated: win.saveBookmark() }
    Repeater {
        model: 9
        Item {
            required property int index
            Shortcut { sequence: "Alt+" + (index + 1); enabled: win.canBookmark && index < engineClient.cameraBookmarks.length; onActivated: win.recallBookmark(index) }
        }
    }
    Shortcut { sequence: "Ctrl+Shift+L"; onActivated: win.toggleTheme() }
    Shortcut { sequences: [StandardKey.ZoomIn]; onActivated: prefs.textScale = prefs.textScale + 0.1 }
    Shortcut { sequences: [StandardKey.ZoomOut]; onActivated: prefs.textScale = prefs.textScale - 0.1 }
    Repeater {
        model: 8
        delegate: Item {
            required property int index
            Shortcut { sequence: "Ctrl+" + (index + 1); enabled: engineClient.projectOpen; onActivated: prefs.workspace = index }
        }
    }

    FolderDialog {
        id: openDialog
        title: Strings.t("Open Project…")
        onAccepted: engineClient.openProject(win.urlToPath(selectedFolder))
    }
    NewProjectDialog { id: newDialog; objectName: "newProjectDialog"; anchors.centerIn: parent }
    CommandPalette { id: palette; commands: win.commands }

    Dialog {
        id: unsavedDialog
        property bool quitAfter: false
        anchors.centerIn: parent
        modal: true
        width: 380
        title: ""
        padding: 20
        background: Rectangle { color: Theme.surfaceElevated; radius: Theme.radiusLarge; border.color: Theme.borderStrong }
        contentItem: ColumnLayout {
            spacing: 14
            Text { text: Strings.t("Save changes before closing?"); color: Theme.textPrimary; font.pixelSize: Theme.fontTitle; font.weight: Font.DemiBold; Layout.fillWidth: true; wrapMode: Text.Wrap }
            Text { text: engineClient.undoLabel ? ("Last change: " + engineClient.undoLabel) : ""; color: Theme.textSecondary; font.pixelSize: Theme.fontSmall; Layout.fillWidth: true; elide: Text.ElideRight }
            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                TButton { text: Strings.t("Don't Save"); destructive: true; onClicked: { unsavedDialog.close(); if (unsavedDialog.quitAfter) { win.closeConfirmed = true; Qt.quit() } else engineClient.closeProject(true) } }
                Item { Layout.fillWidth: true }
                TButton { text: Strings.t("Cancel"); onClicked: unsavedDialog.close() }
                TButton {
                    text: Strings.t("Save"); primary: true
                    onClicked: {
                        unsavedDialog.close()
                        engineClient.saveProject()
                        saveThenClose.quitAfter = unsavedDialog.quitAfter
                        saveThenClose.enabled = true
                    }
                }
            }
        }
    }
    Connections {
        id: saveThenClose
        property bool quitAfter: false
        enabled: false
        target: engineClient
        function onStateChanged() {
            if (engineClient.dirty) return
            saveThenClose.enabled = false
            if (saveThenClose.quitAfter) { win.closeConfirmed = true; Qt.quit() } else engineClient.closeProject(false)
        }
    }

    // Transient notification (engine-originated messages and errors)
    Rectangle {
        id: toast
        property string level: engineClient.messageLevel
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 78
        width: Math.min(parent.width - 80, toastText.implicitWidth + 36)
        height: toastText.implicitHeight + 18
        radius: Theme.radiusLarge
        color: Theme.surfaceElevated
        border.color: level === "error" ? Theme.error : level === "warning" ? Theme.warning : Theme.borderStrong
        opacity: 0
        visible: opacity > 0
        Text {
            id: toastText
            anchors.centerIn: parent
            width: Math.min(implicitWidth, win.width - 116)
            text: engineClient.lastMessage
            color: Theme.textPrimary
            font.pixelSize: Theme.fontBody
            wrapMode: Text.Wrap
        }
        Behavior on opacity { NumberAnimation { duration: Theme.durMed } }
        Timer { id: toastTimer; interval: toast.level === "error" ? 7000 : 2600; onTriggered: toast.opacity = 0 }
        Connections {
            target: engineClient
            function onMessageChanged() { toast.opacity = 1; toastTimer.restart() }
        }
    }
}
