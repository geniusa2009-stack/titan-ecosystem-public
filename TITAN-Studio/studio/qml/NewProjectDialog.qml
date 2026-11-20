// TITAN Studio — New Project dialog.
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Dialogs
import QtQuick.Layouts
import TitanStudio

Popup {
    id: dlg
    width: 460
    modal: true
    focus: true
    padding: 22
    closePolicy: Popup.CloseOnEscape
    Overlay.modal: Rectangle { color: Theme.scrim }
    background: Rectangle { color: Theme.surfaceElevated; radius: Theme.radiusLarge; border.color: Theme.borderStrong }

    property string location: ""
    property var templates: []
    onOpened: {
        nameField.text = "Untitled World"; nameField.selectAll(); nameField.forceActiveFocus()
        templates = []
        if (engineClient.capabilities.indexOf("templates") >= 0)
            engineClient.request("templates.list", {}, (r, err) => { if (!err) dlg.templates = r.templates })
    }

    function create() {
        const name = nameField.text.trim()
        if (!name.length || !location.length) return
        const safe = name.replace(/[\/\\:]/g, "-")
        engineClient.createProject(location + "/" + safe, name, templateBox.currentIndex >= 0 && templates.length ? templates[templateBox.currentIndex].name : "")
        close()
    }

    FolderDialog {
        id: folder
        onAccepted: dlg.location = win.urlToPath(selectedFolder)
    }

    contentItem: ColumnLayout {
        spacing: 12
        Text { text: Strings.t("New Project…"); color: Theme.textPrimary; font.pixelSize: Theme.fontHeadline; font.weight: Font.DemiBold }
        Text { text: Strings.t("Project name"); color: Theme.textSecondary; font.pixelSize: Theme.fontSmall }
        TTextField { id: nameField; Layout.fillWidth: true; Keys.onReturnPressed: dlg.create() }
        Text { visible: dlg.templates.length > 0; text: "Template"; color: Theme.textSecondary; font.pixelSize: Theme.fontSmall }
        ComboBox {
            id: templateBox
            objectName: "templateBox"
            visible: dlg.templates.length > 0
            Layout.fillWidth: true
            model: dlg.templates.map(t => t.name + (t.builtin ? "" : " (yours)") + " · " + t.objects + " objects")
        }
        Text {
            visible: dlg.templates.length > 0 && templateBox.currentIndex >= 0
            text: visible ? dlg.templates[templateBox.currentIndex].description : ""
            color: Theme.textTertiary; font.pixelSize: Theme.fontSmall; wrapMode: Text.Wrap; Layout.fillWidth: true
        }
        Text { text: Strings.t("Location"); color: Theme.textSecondary; font.pixelSize: Theme.fontSmall }
        RowLayout {
            Layout.fillWidth: true
            TTextField { Layout.fillWidth: true; text: dlg.location; readOnly: true; mono: true; placeholderText: "—" }
            TButton { text: Strings.t("Choose…"); onClicked: folder.open() }
        }
        Text {
            text: dlg.location.length ? ("Creates " + dlg.location + "/" + nameField.text.trim().replace(/[\/\\:]/g, "-")) : "Choose a folder for the new project."
            color: Theme.textTertiary; font.pixelSize: Theme.fontSmall; elide: Text.ElideMiddle; Layout.fillWidth: true
        }
        RowLayout {
            Layout.fillWidth: true
            Layout.topMargin: 6
            Item { Layout.fillWidth: true }
            TButton { text: Strings.t("Cancel"); onClicked: dlg.close() }
            TButton { text: Strings.t("Create"); primary: true; enabled: nameField.text.trim().length > 0 && dlg.location.length > 0; onClicked: dlg.create() }
        }
    }
}
