import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "components"

// 搜索浮窗（2026-11-19 UI 改版）
//
// 结构：搜索行 56 / 过滤 Chip 40 / 结果列表 / 底部键位 36。
// 视觉：单层浮窗 + 1px 边框 + 选中行 accentSoft + 左侧 2px 指示条。
// 行为保持原样：失焦 dock、点击两次执行、Alt+Space 唤起、Esc 关闭。
ApplicationWindow {
    id: window
    width: 600
    height: 420
    title: "Assistant"
    flags: Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint | Qt.Tool
    color: "transparent"

    visible: backend.visible
    property bool ready: false

    // ---- QQ-style dock-to-top-edge ----
    property bool docked: false
    property int dockRestoreY: 0
    readonly property int dockStrip: 8

    // 第一次点击只选中，第二次才执行
    property int mouseSelectedIndex: -1

    readonly property bool isDark: Style.isDark

    // ---- 结果类型 → 图标 / 颜色 ----
    function typeIcon(t) {
        switch (t) {
            case "app":        return "app"
            case "file":       return "file"
            case "calculator": return "calc"
            case "url":        return "link"
            default:           return "zap"
        }
    }

    function typeColor(t) {
        switch (t) {
            case "app":        return Style.tileApp
            case "file":       return Style.tileFile
            case "calculator": return Style.tileCalc
            case "url":        return Style.tileCalc
            default:           return Style.text3
        }
    }

    function typeBg(t) {
        switch (t) {
            case "app":        return Style.tileAppBg
            case "file":       return Style.tileFileBg
            case "calculator": return Style.tileCalcBg
            case "url":        return Style.tileCalcBg
            default:           return Style.surface2
        }
    }

    // ---- Phase B：种类过滤 Chip ----
    readonly property var kindTokens: [":doc", ":img", ":vid", ":aud",
                                       ":zip", ":exe", ":code", ":pdf"]

    function isKindToken(text) {
        return kindTokens.indexOf(String(text).toLowerCase()) >= 0
    }

    function chipActive(token) {
        var parts = searchInput.text.toLowerCase().split(/\s+/)
        if (token === "") {
            for (var i = 0; i < parts.length; ++i)
                if (isKindToken(parts[i])) return false
            return true
        }
        return parts.indexOf(token) >= 0
    }

    function toggleKindChip(token) {
        var parts = searchInput.text.split(/\s+/).filter(function(p) { return p.length > 0 })
        if (token === "") {
            parts = parts.filter(function(p) { return !isKindToken(p) })
        } else {
            var at = -1
            for (var i = 0; i < parts.length; ++i)
                if (String(parts[i]).toLowerCase() === token) at = i
            if (at >= 0) parts.splice(at, 1)
            else parts.push(token)
        }
        searchInput.text = parts.join(" ")
        searchInput.cursorPosition = searchInput.text.length
    }

    function moveSelection(delta) {
        if (resultsList.count === 0) return
        var next = resultsList.currentIndex + delta
        if (next < 0) next = 0
        if (next > resultsList.count - 1) next = resultsList.count - 1
        resultsList.currentIndex = next
        window.mouseSelectedIndex = -1
    }

    function executeCurrent() {
        if (resultsList.currentIndex >= 0 && resultsList.count > 0)
            backend.executeCommand(resultsList.currentIndex)
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

    Connections {
        target: backend

        function onVisibleChanged() {
            if (backend.visible) {
                docked = false
                mouseSelectedIndex = -1
                if (config.windowX >= 0 && config.windowY >= 0) {
                    window.x = config.windowX
                    window.y = config.windowY
                } else {
                    window.x = (Screen.width - window.width) / 2
                    window.y = 0
                }
                searchInput.forceActiveFocus()
                searchInput.text = ""
                backend.activate()
            }
        }

        function onCommandExecuted(command) {
            backend.hideWindow()
        }
    }

    onActiveChanged: {
        if (ready && !active && visible)
            dock()
    }

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

    // 鼠标移到顶部露出的细条 → 展开
    MouseArea {
        anchors.fill: parent
        hoverEnabled: true
        onEntered: { if (docked) undock() }
    }

    Rectangle {
        id: shell
        anchors.fill: parent
        radius: 14
        color: Style.surface
        border.width: 1
        border.color: Style.lineStrong
        focus: true

        // dock 后露出的顶部提示线
        Rectangle {
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right
            height: 2
            radius: 1
            color: Style.accent
            opacity: window.docked ? 1 : 0
            Behavior on opacity { NumberAnimation { duration: Style.motionFast } }
        }

        Keys.onEscapePressed: backend.hideWindow()
        Keys.onReturnPressed: window.executeCurrent()
        Keys.onUpPressed: window.moveSelection(-1)
        Keys.onDownPressed: window.moveSelection(1)

        // 整窗拖动（放在内容下层，空白处可拖）
        MouseArea {
            anchors.fill: parent
            property real dragX: 0
            property real dragY: 0
            onPressed: function(mouse) { dragX = mouse.x; dragY = mouse.y }
            onPositionChanged: function(mouse) {
                window.x += mouse.x - dragX
                window.y += mouse.y - dragY
            }
        }

        ColumnLayout {
            id: mainColumn
            anchors.fill: parent
            spacing: 0

            // ================= 搜索行 =================
            Item {
                Layout.fillWidth: true
                Layout.preferredHeight: 57

                MouseArea {
                    anchors.fill: parent
                    property real lastX: 0
                    property real lastY: 0
                    onPressed: function(mouse) { lastX = mouse.x; lastY = mouse.y }
                    onPositionChanged: function(mouse) {
                        window.x += mouse.x - lastX
                        window.y += mouse.y - lastY
                    }
                }

                AppIcon {
                    id: searchIcon
                    anchors.left: parent.left
                    anchors.leftMargin: 16
                    anchors.verticalCenter: parent.verticalCenter
                    name: "search"
                    size: 18
                    weight: 2
                    iconColor: Style.accent
                }

                RowLayout {
                    id: searchHints
                    anchors.right: parent.right
                    anchors.rightMargin: 10
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 5

                    AppKbd { text: "Alt"; Layout.alignment: Qt.AlignVCenter }
                    AppKbd { text: "Space"; Layout.alignment: Qt.AlignVCenter }
                    AppIconButton {
                        icon: "home"
                        iconSize: 15
                        tooltip: "打开主面板"
                        onClicked: backend.openMainPanel()
                    }
                }

                TextInput {
                    id: searchInput
                    anchors.left: searchIcon.right
                    anchors.leftMargin: 12
                    anchors.right: searchHints.left
                    anchors.rightMargin: 10
                    anchors.verticalCenter: parent.verticalCenter
                    height: 28
                    color: Style.text
                    font.pixelSize: 15
                    font.family: Style.fontFamily
                    selectionColor: Style.accent
                    selectedTextColor: Style.textOnAccent
                    focus: true
                    activeFocusOnTab: false
                    verticalAlignment: TextInput.AlignVCenter
                    clip: true

                    Text {
                        anchors.fill: parent
                        visible: !searchInput.text
                        text: "搜索应用、文件、计算…"
                        color: Style.text3
                        font: searchInput.font
                        verticalAlignment: Text.AlignVCenter
                    }

                    Timer {
                        id: searchDebounce
                        interval: 40
                        repeat: false
                        onTriggered: backend.search(searchInput.text)
                    }

                    onTextChanged: {
                        if (searchInput.inputMethodComposing)
                            return
                        resultsList.currentIndex = 0
                        window.mouseSelectedIndex = -1
                        searchDebounce.restart()
                    }

                    Keys.onEscapePressed: backend.hideWindow()
                    Keys.onUpPressed: window.moveSelection(-1)
                    Keys.onDownPressed: window.moveSelection(1)
                }
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 1
                color: Style.line
            }

            // ================= 过滤 Chip =================
            Item {
                Layout.fillWidth: true
                Layout.preferredHeight: 40

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 12
                    anchors.rightMargin: 12
                    spacing: 4

                    Repeater {
                        model: [
                            { "token": "",      "label": "全部" },
                            { "token": ":doc",  "label": "文档" },
                            { "token": ":img",  "label": "图片" },
                            { "token": ":vid",  "label": "视频" },
                            { "token": ":aud",  "label": "音频" },
                            { "token": ":zip",  "label": "压缩" },
                            { "token": ":exe",  "label": "安装包" },
                            { "token": ":code", "label": "代码" },
                            { "token": ":pdf",  "label": "PDF" }
                        ]

                        delegate: AppChip {
                            text: modelData.label
                            selected: window.chipActive(modelData.token)
                            onClicked: window.toggleKindChip(modelData.token)
                        }
                    }

                    Item { Layout.fillWidth: true }
                }
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 1
                color: Style.line
            }

            // ================= 结果列表 =================
            ListView {
                id: resultsList
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.preferredHeight: 1
                Layout.minimumHeight: 0
                clip: true
                model: backend.searchResults
                currentIndex: 0
                spacing: 2
                topMargin: 4
                bottomMargin: 4

                onCurrentIndexChanged: {
                    if (currentIndex >= 0)
                        positionViewAtIndex(currentIndex, ListView.Contain)
                }

                delegate: Rectangle {
                    id: row
                    width: ListView.view.width - 12
                    x: 6
                    height: 52
                    radius: 9
                    color: index === resultsList.currentIndex
                           ? Style.accentSoft
                           : hover.hovered ? Style.surface2 : "transparent"
                    Behavior on color { ColorAnimation { duration: Style.motionFast } }

                    HoverHandler { id: hover }

                    Rectangle {
                        visible: index === resultsList.currentIndex
                        anchors.left: parent.left
                        anchors.verticalCenter: parent.verticalCenter
                        width: 2
                        height: 22
                        radius: 1
                        color: Style.accent
                    }

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 10
                        anchors.rightMargin: 10
                        spacing: 10

                        Rectangle {
                            Layout.preferredWidth: 30
                            Layout.preferredHeight: 30
                            Layout.alignment: Qt.AlignVCenter
                            radius: 9
                            color: window.typeBg(modelData.type)

                            AppIcon {
                                anchors.centerIn: parent
                                name: window.typeIcon(modelData.type)
                                size: 15
                                iconColor: window.typeColor(modelData.type)
                            }
                        }

                        ColumnLayout {
                            Layout.fillWidth: true
                            Layout.alignment: Qt.AlignVCenter
                            spacing: 2

                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 8

                                Text {
                                    Layout.fillWidth: true
                                    text: modelData.title
                                    color: Style.text
                                    font.pixelSize: 13
                                    font.weight: Font.DemiBold
                                    font.family: Style.fontFamily
                                    elide: Text.ElideRight
                                }

                                Text {
                                    visible: modelData.result !== ""
                                    text: modelData.result
                                    color: Style.accent
                                    font.pixelSize: 13
                                    font.weight: Font.DemiBold
                                    font.family: Style.fontFamily
                                }
                            }

                            Text {
                                Layout.fillWidth: true
                                visible: modelData.subtitle !== ""
                                text: modelData.subtitle
                                color: Style.text3
                                font.pixelSize: Style.fontMicro
                                font.family: Style.fontFamily
                                elide: Text.ElideRight
                            }
                        }

                        AppKbd {
                            text: "↵"
                            visible: index === resultsList.currentIndex
                        }
                    }

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

                AppEmptyState {
                    anchors.centerIn: parent
                    width: 300
                    height: 150
                    visible: resultsList.count === 0
                    icon: "search"
                    title: searchInput.text.length > 0 ? "没有找到相关结果" : "输入关键词开始搜索"
                    hint: searchInput.text.length > 0
                          ? "试试去掉 :doc 之类的过滤，或换一个更短的关键词"
                          : "支持应用、文件、计算式；也可以用 :doc / in:downloads 过滤"
                }
            }

            // ================= 底部状态 / 键位 =================
            Rectangle {
                id: footer
                Layout.fillWidth: true
                Layout.preferredHeight: 36
                color: Style.surface

                Rectangle {
                    anchors.top: parent.top
                    anchors.left: parent.left
                    anchors.right: parent.right
                    height: 1
                    color: Style.line
                }

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 14
                    anchors.rightMargin: 12
                    spacing: 8

                    Text {
                        Layout.fillWidth: true
                        text: backend.statusText
                        color: Style.text2
                        font.pixelSize: Style.fontMicro
                        font.family: Style.fontFamily
                        elide: Text.ElideRight
                    }

                    AppKbd { text: "↑" }
                    AppKbd { text: "↓" }
                    Text {
                        text: "选择"
                        color: Style.text3
                        font.pixelSize: 9
                        font.family: Style.fontFamily
                    }
                    AppKbd { text: "↵" }
                    Text {
                        text: "打开"
                        color: Style.text3
                        font.pixelSize: 9
                        font.family: Style.fontFamily
                    }
                    AppKbd { text: "Esc" }
                    Text {
                        text: "关闭"
                        color: Style.text3
                        font.pixelSize: 9
                        font.family: Style.fontFamily
                    }
                }
            }
        }
    }
}
