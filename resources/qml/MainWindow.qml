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

    // ---- QQ-style dock-to-top-edge ----
    // On focus loss the window slides up so only a thin strip stays visible at
    // the top of the screen; hovering that strip slides it back down.
    property bool docked: false
    property int  dockRestoreY: 0
    readonly property int dockStrip: 8

    // Mouse click-to-select state: first click selects, second click executes.
    // Kept separate from currentIndex so keyboard/default pre-selection doesn't
    // cause a first click to launch immediately.
    property int mouseSelectedIndex: -1

    // ---- Theme colors：统一来自 Style 单例（同目录 qmldir 的 singleton）----
    readonly property bool isDark:  Style.isDark
    readonly property color bgColor:         Style.bgColor
    readonly property color panelColor:      Style.panelColor
    readonly property color textColor:       Style.textColor
    readonly property color mutedColor:      Style.mutedColor
    readonly property color accentColor:     Style.accentColor
    readonly property color borderColor:     Style.borderColor
    readonly property color activeBorder:    Style.activeBorder
    readonly property color statusTextColor: Style.statusTextColor

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

    function dock() {
        if (docked) return
        docked = true
        dockRestoreY = window.y
        dockAnim.from = window.y
        dockAnim.to = -(window.height - dockStrip)
        dockAnim.restart()
    }

    function undock() {
        if (!docked) return
        docked = false
        undockAnim.from = window.y
        undockAnim.to = dockRestoreY
        undockAnim.restart()
        window.requestActivate()
        searchInput.forceActiveFocus()
    }

    // ---- C++ signal handlers ----
    Connections {
        target: backend

        function onVisibleChanged() {
            if (backend.visible) {
                docked = false
                mouseSelectedIndex = -1
                // Restore saved position, or center on first run
                if (config.windowX >= 0 && config.windowY >= 0) {
                    window.x = config.windowX
                    window.y = config.windowY
                } else {
                    // Default: horizontally centered, flush against the top edge
                    window.x = (Screen.width - window.width) / 2
                    window.y = 0
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

    // Click outside → dock to the top edge (QQ-style), not fully hide
    onActiveChanged: {
        if (ready && !active && visible) {
            dock()
        }
    }

    // ---- Dock slide animations ----
    PropertyAnimation {
        id: dockAnim
        target: window
        property: "y"
        duration: 200
        easing.type: Easing.OutCubic
    }

    PropertyAnimation {
        id: undockAnim
        target: window
        property: "y"
        duration: 200
        easing.type: Easing.OutCubic
    }

    // Hover trigger — sits behind the content. When docked, the only on-screen
    // part is the strip at the top, so onEntered == "cursor touched the strip".
    MouseArea {
        anchors.fill: parent
        hoverEnabled: true
        onEntered: { if (docked) undock() }
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
                window.mouseSelectedIndex = -1
            }
        }

        Keys.onDownPressed: {
            if (resultsList.currentIndex < resultsList.count - 1) {
                resultsList.currentIndex++
                window.mouseSelectedIndex = -1
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

                        // A1 修 P1-6：不再逐字检索。
                        //  - IME 预编辑阶段（拼音串）不该触发检索 → inputMethodComposing 时直接跳过
                        //  - 连续输入用 40ms 防抖合并：连打 8 个字符只跑 1 次全量模糊匹配
                        Timer {
                            id: searchDebounce
                            interval: 40
                            repeat: false
                            onTriggered: backend.search(searchInput.text)
                        }

                        onTextChanged: {
                            if (searchInput.inputMethodComposing)
                                return

                            // New query → reset selection to top result
                            resultsList.currentIndex = 0
                            window.mouseSelectedIndex = -1
                            searchDebounce.restart()
                        }
                    }

                    // 🏠 按钮：打开主面板（Phase 3 Step 1）
                    Rectangle {
                        Layout.preferredWidth: 30
                        Layout.preferredHeight: 30
                        radius: 6
                        color: homeMouse.containsMouse ? panelColor : "transparent"

                        Text {
                            anchors.centerIn: parent
                            text: "🏠"
                            font.pixelSize: 16
                        }

                        MouseArea {
                            id: homeMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: backend.openMainPanel()
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

                    // Mouse selection: click to select, click again to launch
                    MouseArea {
                        anchors.fill: parent
                        onClicked: {
                            if (window.mouseSelectedIndex === index) {
                                backend.executeCommand(index)
                                window.mouseSelectedIndex = -1
                            } else {
                                window.mouseSelectedIndex = index
                                resultsList.currentIndex = index
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
