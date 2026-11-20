// TITAN Studio — project launcher. Original TITAN design.
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import TitanStudio

Item {
    id: root
    signal newProject()
    signal openProject()

    RowLayout {
        anchors.fill: parent
        spacing: 0

        // Left: identity and primary actions
        Rectangle {
            Layout.fillHeight: true
            Layout.preferredWidth: 340
            color: Theme.sidebar
            Rectangle { anchors.right: parent.right; width: 1; height: parent.height; color: Theme.border }

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 36
                spacing: 0

                Item { Layout.preferredHeight: 28 }
                // Wordmark: original TITAN mark (geometric "T" in a rounded square)
                Rectangle {
                    width: 56; height: 56; radius: 14
                    gradient: Gradient {
                        GradientStop { position: 0; color: Qt.lighter(Theme.accent, 1.12) }
                        GradientStop { position: 1; color: Qt.darker(Theme.accent, 1.25) }
                    }
                    Rectangle { x: 14; y: 15; width: 28; height: 6; radius: 2; color: "white" }
                    Rectangle { x: 25; y: 15; width: 6; height: 26; radius: 2; color: "white" }
                }
                Item { Layout.preferredHeight: 22 }
                Text { text: "TITAN"; color: Theme.textPrimary; font.pixelSize: Theme.fontDisplay; font.weight: Font.Bold; font.letterSpacing: 6 }
                Text { text: "Studio"; color: Theme.textSecondary; font.pixelSize: Theme.fontHeadline; font.weight: Font.Light; font.letterSpacing: 2 }
                Item { Layout.preferredHeight: 8 }
                Text { text: "Engine " + titanVersion + "  ·  protocol " + (engineClient.engineInfo.protocol || "–"); color: Theme.textTertiary; font.pixelSize: Theme.fontSmall; font.family: Theme.monoFamily }
                Item { Layout.preferredHeight: 36 }

                TButton { text: Strings.t("New Project…"); primary: true; Layout.fillWidth: true; enabled: engineClient.connected; onClicked: root.newProject() }
                Item { Layout.preferredHeight: 10 }
                TButton { text: Strings.t("Open Project…"); Layout.fillWidth: true; enabled: engineClient.connected; onClicked: root.openProject() }

                Item { Layout.fillHeight: true }

                SectionHeader { title: Strings.t("Appearance"); Layout.fillWidth: true }
                Item { Layout.preferredHeight: 8 }
                Segmented {
                    Layout.fillWidth: true
                    model: [Strings.t("System"), Strings.t("Light"), Strings.t("Dark")]
                    currentIndex: ["system", "light", "dark"].indexOf(prefs.themeMode)
                    onActivated: (i) => prefs.themeMode = ["system", "light", "dark"][i]
                }
                Item { Layout.preferredHeight: 14 }
                SectionHeader { title: Strings.t("Language"); Layout.fillWidth: true }
                Item { Layout.preferredHeight: 8 }
                Segmented {
                    Layout.fillWidth: true
                    model: ["English", "العربية"]
                    currentIndex: prefs.language === "ar" ? 1 : 0
                    onActivated: (i) => prefs.language = i === 1 ? "ar" : "en"
                }
                Item { Layout.preferredHeight: 14 }
                SectionHeader { title: Strings.t("Engine"); Layout.fillWidth: true }
                Item { Layout.preferredHeight: 8 }
                Segmented {
                    id: engineChoice
                    Layout.fillWidth: true
                    model: [Strings.t("TITAN Reference"), Strings.t("Unreal Engine 5.8.4")]
                    currentIndex: engineClient.engineKind === "unreal" ? 1 : 0
                    onActivated: (i) => {
                        const kind = i === 1 ? "unreal" : "reference"
                        if (kind === engineClient.engineKind) return
                        if (kind === "unreal" && !engineClient.unrealAvailable) {
                            currentIndex = 0
                            engineClient.notify(Strings.t("Unreal Editor is not built yet (scripts/ue_build.sh build)."), "warning")
                            return
                        }
                        prefs.engineKind = kind
                        engineClient.engineKind = kind
                        engineClient.restart()
                    }
                }
                Text {
                    visible: !engineClient.unrealAvailable
                    text: Strings.t("Unreal Editor is not built yet (scripts/ue_build.sh build).")
                    color: Theme.textTertiary; font.pixelSize: Theme.fontSmall; wrapMode: Text.Wrap; Layout.fillWidth: true
                    Layout.topMargin: 6
                }
                Item { Layout.preferredHeight: 22 }
                RowLayout {
                    spacing: 8
                    Rectangle { width: 8; height: 8; radius: 4; color: engineClient.connected ? Theme.success : Theme.warning }
                    Text { text: engineClient.connectionStatus; color: Theme.textSecondary; font.pixelSize: Theme.fontSmall; Layout.fillWidth: true; elide: Text.ElideRight }
                    TButton { text: Strings.t("Restart Engine"); subtle: true; visible: !engineClient.connected; onClicked: engineClient.restart() }
                }
            }
        }

        // Right: recent projects
        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.margins: 40
            spacing: 14
            Text { text: Strings.t("Recent Projects"); color: Theme.textPrimary; font.pixelSize: Theme.fontLarge; font.weight: Font.DemiBold }
            Rectangle {
                Layout.fillWidth: true
                Layout.fillHeight: true
                radius: Theme.radiusLarge
                color: Theme.surface
                border.color: Theme.border

                ListView {
                    id: recent
                    anchors.fill: parent
                    anchors.margins: 6
                    clip: true
                    model: prefs.recentProjects
                    spacing: 2
                    keyNavigationEnabled: true
                    focus: true
                    delegate: Rectangle {
                        id: row
                        required property var modelData
                        required property int index
                        width: recent.width
                        height: 56
                        radius: Theme.radius
                        color: recent.currentIndex === index && recent.activeFocus ? Theme.selection : hover.hovered ? Theme.hover : "transparent"
                        HoverHandler { id: hover }
                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: 14
                            anchors.rightMargin: 10
                            spacing: 12
                            Rectangle {
                                width: 32; height: 32; radius: 8
                                color: Theme.surfaceSecondary
                                Text { anchors.centerIn: parent; text: (row.modelData.name || "?").charAt(0).toUpperCase(); color: Theme.accentStrong; font.pixelSize: Theme.fontTitle; font.weight: Font.Bold }
                            }
                            ColumnLayout {
                                spacing: 2
                                Layout.fillWidth: true
                                Text { text: row.modelData.name; color: row.modelData.exists ? Theme.textPrimary : Theme.textDisabled; font.pixelSize: Theme.fontTitle; font.weight: Font.Medium; elide: Text.ElideRight; Layout.fillWidth: true }
                                Text { text: row.modelData.path; color: Theme.textTertiary; font.pixelSize: Theme.fontSmall; elide: Text.ElideMiddle; Layout.fillWidth: true; LayoutMirroring.enabled: false }
                            }
                            Text { text: row.modelData.exists ? Qt.formatDateTime(new Date(row.modelData.opened), "d MMM yyyy") : Strings.t("Missing"); color: row.modelData.exists ? Theme.textTertiary : Theme.warning; font.pixelSize: Theme.fontSmall }
                            TIconButton { glyph: "✕"; tip: Strings.t("Remove"); visible: hover.hovered; onClicked: prefs.removeRecent(row.modelData.path) }
                        }
                        TapHandler {
                            onTapped: recent.currentIndex = row.index
                            onDoubleTapped: if (row.modelData.exists && engineClient.connected) engineClient.openProject(row.modelData.path)
                        }
                    }
                    Keys.onReturnPressed: if (currentIndex >= 0 && model[currentIndex].exists) engineClient.openProject(model[currentIndex].path)
                }
                ColumnLayout {
                    anchors.centerIn: parent
                    visible: recent.count === 0
                    spacing: 6
                    Text { text: Strings.t("No recent projects yet."); color: Theme.textSecondary; font.pixelSize: Theme.fontTitle; Layout.alignment: Qt.AlignHCenter }
                    Text { text: Strings.t("Create a project to get started."); color: Theme.textTertiary; font.pixelSize: Theme.fontBody; Layout.alignment: Qt.AlignHCenter }
                }
            }
        }
    }
}
