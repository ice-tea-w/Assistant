import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ApplicationWindow {
    id: window
    width: 600
    height: 420
    title: "Assistant"
    flags: Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint | Qt.Tool
    color: "transparent"

    // ---- C++ property binding ----
    // Window visibility is driven by the C++ AppBridge::visible property.
    // QML only calls backend.show/hide/toggle — never sets visible directly.
    visible: backend.visible

    property bool ready: false

    // ---- Theme colors (driven by C++ ConfigManager::theme property) ----
    readonly property bool isDark:  config.theme === "dark"
    readonly property color bgColor:         isDark ? "#2c2c2c" : "#f0f0f0"
    readonly property color panelColor:      isDark ? "#1e1e1e" : "#ffffff"
    readonly property color textColor:       isDark ? "#e0e0e0" : "#1a1a1a"
    readonly property color mutedColor:      isDark ? "#666666" : "#999999"
    readonly property color accentColor:     isDark ? "#3a6ea5" : "#4a90d9"
    readonly property color borderColor:     isDark ? "#3d3d3d" : "#d0d0d0"
    readonly property color activeBorder:    isDark ? "#5a8ec9" : "#4a90d9"
    readonly property color statusTextColor: isDark ? "#888888" : "#777777"

    // ---- Result type → icon glyph + badge color ----
    // Single source of truth for how each result category is visualized.
    // Kept as functions (not readonly properties) so the delegate can call
    // them with modelData.type directly.
    function typeGlyph(t) {
        switch (t) {
            case "app":        return "▶"
            case "file":       return "▤"
            case "calculator": return "#"
            case "url":        return "↗"
            default:           return "⚡"
        }
    }

    function typeColor(t) {
        switch (t) {
            case "app":        return accentColor
            case "file":       return isDark ? "#5b8f5b" : "#4c8a4c"
            case "calculator": return isDark ? "#8f6bd1" : "#7a58c4"
            case "url":        return isDark ? "#3d9ba8" : "#2f8b98"
            default:           return isDark ? "#5a5a5a" : "#a0a0a0"
        }
    }

    Component.onCompleted: ready = true

    // ---- C++ signal handlers ----
    Connections {
        target: backend

        function onVisibleChanged() {
            if (backend.visible) {
                // Restore saved position, or center on first run
                if (config.windowX >= 0 && config.windowY >= 0) {
                    window.x = config.windowX
                    window.y = config.windowY
                } else {
                    window.x = (Screen.width  - window.width)  / 2
                    window.y =  Screen.height / 4
                }
                searchInput.forceActiveFocus()
                searchInput.text = ""
                backend.activate()
            }
        }

        function onCommandExecuted(command) {
            // Command was dispatched in C++; hide the window
            backend.hideWindow()
        }
    }

    // Click outside → hide (routes through C++ so state stays in sync)
    onActiveChanged: {
        if (ready && !active && visible) {
            backend.hideWindow()
        }
    }

    Rectangle {
        anchors.fill: parent
        radius: 12
        color: bgColor
        border.color: borderColor
        border.width: 1
        focus: true

        Keys.onEscapePressed: {
            backend.hideWindow()
        }

        Keys.onReturnPressed: {
            if (resultsList.currentIndex >= 0 && resultsList.count > 0) {
                backend.executeCommand(resultsList.currentIndex)
            }
        }

        Keys.onUpPressed: {
            if (resultsList.currentIndex > 0) {
                resultsList.currentIndex--
            }
        }

        Keys.onDownPressed: {
            if (resultsList.currentIndex < resultsList.count - 1) {
                resultsList.currentIndex++
            }
        }

        // ---- Whole-window drag (behind ColumnLayout) ----
        // ColumnLayout is on top and consumes clicks on controls;
        // clicks on empty areas fall through to here.
        MouseArea {
            anchors.fill: parent
            property real dragX: 0
            property real dragY: 0

            onPressed: function(mouse) {
                dragX = mouse.x
                dragY = mouse.y
            }

            onPositionChanged: function(mouse) {
                window.x += mouse.x - dragX
                window.y += mouse.y - dragY
            }
        }

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 10
            spacing: 8

            // ---- Search bar (also drag handle) ----
            Rectangle {
                id: searchBar
                Layout.fillWidth: true
                Layout.preferredHeight: 42
                radius: 8
                color: panelColor
                border.color: searchInput.activeFocus ? activeBorder : borderColor

                // Search bar drag — behind RowLayout, catches clicks on icon / margins
                MouseArea {
                    anchors.fill: parent
                    property real lastX: 0
                    property real lastY: 0

                    onPressed: function(mouse) {
                        lastX = mouse.x
                        lastY = mouse.y
                    }

                    onPositionChanged: function(mouse) {
                        window.x += mouse.x - lastX
                        window.y += mouse.y - lastY
                    }
                }

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 14
                    anchors.rightMargin: 14
                    spacing: 10

                    Text {
                        text: "⌕"
                        font.pixelSize: 18
                        color: mutedColor
                    }

                    TextInput {
                        id: searchInput
                        Layout.fillWidth: true
                        color: textColor
                        font.pixelSize: 16
                        focus: true
                        activeFocusOnTab: false

                        Text {
                            anchors.fill: parent
                            visible: !searchInput.text && !searchInput.activeFocus
                            text: "Type to search..."
                            color: mutedColor
                            font: searchInput.font
                        }

                        onTextChanged: {
                            backend.search(text)
                            // New query → reset selection to top result
                            resultsList.currentIndex = 0
                        }
                    }
                }
            }

            // ---- Results list ----
            ListView {
                id: resultsList
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                model: backend.searchResults
                currentIndex: 0
                spacing: 2

                // Keep the highlighted item visible as keyboard navigation moves it
                onCurrentIndexChanged: {
                    if (currentIndex >= 0)
                        positionViewAtIndex(currentIndex, ListView.Contain)
                }

                delegate: Rectangle {
                    width: ListView.view.width
                    height: 52
                    radius: 6
                    color: index === resultsList.currentIndex ? accentColor : "transparent"

                    Behavior on color { ColorAnimation { duration: 100 } }

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 14
                        anchors.rightMargin: 14
                        spacing: 10

                        // Type icon — colored badge, one tint per result category
                        Rectangle {
                            Layout.preferredWidth: 26
                            Layout.preferredHeight: 26
                            radius: 6
                            color: typeColor(modelData.type)
                            Layout.alignment: Qt.AlignVCenter

                            Text {
                                anchors.centerIn: parent
                                text: typeGlyph(modelData.type)
                                color: "white"
                                font.pixelSize: 13
                                font.bold: true
                            }
                        }

                        Column {
                            Layout.fillWidth: true
                            Layout.alignment: Qt.AlignVCenter
                            spacing: 2

                            RowLayout {
                                spacing: 8

                                Text {
                                    text: modelData.title
                                    color: textColor
                                    font.pixelSize: 14
                                    font.bold: true
                                    elide: Text.ElideRight
                                    Layout.fillWidth: true
                                }

                                Text {
                                    text: modelData.result
                                    color: accentColor
                                    font.pixelSize: 14
                                    font.bold: true
                                    visible: modelData.result !== ""
                                }
                            }

                            Text {
                                text: modelData.subtitle
                                color: mutedColor
                                font.pixelSize: 12
                                elide: Text.ElideRight
                                visible: modelData.subtitle !== ""
                            }
                        }
                    }
                }

                Text {
                    anchors.centerIn: parent
                    text: "No results"
                    color: mutedColor
                    font.pixelSize: 14
                    visible: resultsList.count === 0
                }
            }

            // ---- Status bar (C++ → QML signal/slot demo) ----
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 28
                radius: 6
                color: panelColor

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 12
                    anchors.rightMargin: 12

                    Text {
                        text: backend.statusText
                        color: statusTextColor
                        font.pixelSize: 12
                        elide: Text.ElideRight
                        Layout.fillWidth: true
                    }

                    Text {
                        text: "↑↓ 选择 · Enter 打开 · Esc 关闭"
                        color: statusTextColor
                        font.pixelSize: 11
                        opacity: 0.7
                    }
                }
            }
        }
    }
}
