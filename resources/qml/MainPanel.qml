import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "components"

// 主面板（2026-11-19 UI 改版）
//
// 窗口：Frameless + 自绘 44px 标题栏，四边 6px 可缩放。
// 结构：158px 左侧导航（分组 + 设置固定底部）+ Loader 页面。
// 行为保持：关闭只隐藏、插件页由 C++ 创建、selectedKey 驱动导航。
ApplicationWindow {
    id: panelWindow
    width: 760
    height: 520
    minimumWidth: 720
    minimumHeight: 480
    title: "Assistant"
    color: "transparent"
    flags: Qt.Window | Qt.FramelessWindowHint
    visible: false

    readonly property bool isDark: Style.isDark

    palette.window: Style.bg
    palette.windowText: Style.text
    palette.base: Style.surface
    palette.alternateBase: Style.surface
    palette.text: Style.text
    palette.button: Style.surface2
    palette.buttonText: Style.text
    palette.brightText: "#ffffff"
    palette.highlight: Style.accent
    palette.highlightedText: Style.textOnAccent
    palette.placeholderText: Style.text3
    palette.toolTipBase: Style.surface
    palette.toolTipText: Style.text
    palette.light: Style.line
    palette.midlight: Style.line
    palette.mid: Style.line
    palette.dark: Style.text3
    palette.shadow: "#000000"
    palette.link: Style.accent
    palette.linkVisited: Style.accent

    property string selectedKey: "todo"

    readonly property var pluginApi: (typeof plugins !== "undefined") ? plugins : null

    readonly property var builtinNav: [
        { "key": "todo",     "icon": "todo",     "label": "待办事项" },
        { "key": "notes",    "icon": "note",     "label": "记事本"   },
        { "key": "clip",     "icon": "clip",     "label": "剪贴板"   },
        { "key": "downloads","icon": "download", "label": "最近下载" },
        { "key": "sync",     "icon": "sync",     "label": "设备同步" },
        { "key": "insight",  "icon": "chart",    "label": "数据看板" },
        { "key": "security", "icon": "shield", "label": "安全防护" },
        { "key": "plugins",  "icon": "plugin",   "label": "插件市场" }
    ]

    readonly property var pluginNav: {
        var out = []
        if (!pluginApi || !pluginApi.navigationEntries) return out
        var entries = pluginApi.navigationEntries
        for (var i = 0; i < entries.length; ++i) {
            var e = entries[i]
            out.push({ "key": e.key, "icon": e.icon ? "plugin" : "plugin", "label": e.title })
        }
        return out
    }

    // 导航模型：分组标题 + 导航项 + 插件项
    readonly property var navModel: {
        var out = []
        out.push({ "type": "label", "label": "工作台" })
        for (var i = 0; i < 3 && i < builtinNav.length; ++i)
            out.push({ "type": "item", "key": builtinNav[i].key,
                       "icon": builtinNav[i].icon, "label": builtinNav[i].label })
        out.push({ "type": "label", "label": "效率" })
        for (var j = 3; j < builtinNav.length; ++j)
            out.push({ "type": "item", "key": builtinNav[j].key,
                       "icon": builtinNav[j].icon, "label": builtinNav[j].label })
        for (var k = 0; k < pluginNav.length; ++k)
            out.push({ "type": "item", "key": pluginNav[k].key,
                       "icon": "plugin", "label": pluginNav[k].label })
        return out
    }

    function isPluginKey(key) {
        return typeof key === "string" && key.indexOf(":") > 0
    }

    function indexOfKey(key) {
        for (var i = 0; i < navModel.length; ++i)
            if (navModel[i].type === "item" && navModel[i].key === key) return i
        return -1
    }

    function sourceFor(key) {
        switch (key) {
        case "todo":      return "qrc:/qml/TodoPage.qml"
        case "notes":     return "qrc:/qml/NotesPage.qml"
        case "clip":      return "qrc:/qml/ClipboardPage.qml"
        case "downloads": return "qrc:/qml/RecentDownloadsPage.qml"
        case "sync":      return "qrc:/qml/SyncPage.qml"
        case "insight":   return "qrc:/qml/InsightPage.qml"
        case "security": return "qrc:/qml/SecurityPage.qml"
        case "plugins":   return "qrc:/qml/PluginsPage.qml"
        case "settings":  return "qrc:/qml/SettingsPage.qml"
        }
        return ""
    }

    property var pluginPageItem: null

    function detachAllPluginPages() {
        var slot = pluginPageSlot
        if (!slot) return
        var kids = slot.data
        for (var i = 0; i < kids.length; ++i)
            kids[i].visible = false
    }

    function attachPluginPage() {
        detachAllPluginPages()
        if (!isPluginKey(selectedKey) || indexOfKey(selectedKey) < 0) {
            pluginPageItem = null
            return
        }
        var obj = pluginApi ? pluginApi.acquirePage(selectedKey) : null
        pluginPageItem = obj
        if (!obj) return
        obj.parent = pluginPageSlot
        obj.visible = true
        obj.width = Qt.binding(function() { return pluginPageSlot.width })
        obj.height = Qt.binding(function() { return pluginPageSlot.height })
    }

    onSelectedKeyChanged: attachPluginPage()

    Connections {
        target: panelWindow.pluginApi
        ignoreUnknownSignals: true
        function onNavigationChanged() {
            var key = panelWindow.selectedKey
            if (panelWindow.isPluginKey(key) && panelWindow.indexOfKey(key) < 0)
                panelWindow.selectedKey = "plugins"
            else
                panelWindow.attachPluginPage()
        }
    }

    Component.onCompleted: attachPluginPage()

    onClosing: function(close) {
        close.accepted = false
        panelWindow.hide()
    }

    Rectangle {
        id: shell
        anchors.fill: parent
        // 最大化时去掉圆角和边框，避免屏幕边缘出现缝隙
        radius: panelWindow.visibility === Window.Maximized ? 0 : Style.radiusWindow
        color: Style.bg
        border.width: panelWindow.visibility === Window.Maximized ? 0 : 1
        border.color: Style.lineStrong

        ColumnLayout {
            anchors.fill: parent
            spacing: 0

            // ================= 标题栏 =================
            Item {
                Layout.fillWidth: true
                Layout.preferredHeight: Style.titlebarHeight

                MouseArea {
                    anchors.fill: parent
                    onPressed: panelWindow.startSystemMove()
                    onDoubleClicked: {
                        if (panelWindow.visibility === Window.Maximized)
                            panelWindow.showNormal()
                        else
                            panelWindow.showMaximized()
                    }
                }

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 14
                    anchors.rightMargin: 8
                    spacing: 8

                    Rectangle {
                        width: 21
                        height: 21
                        radius: 6
                        gradient: Gradient {
                            GradientStop { position: 0; color: "#6E8DFF" }
                            GradientStop { position: 1; color: "#9B7BFF" }
                        }
                        Text {
                            anchors.centerIn: parent
                            text: "A"
                            color: "#FFFFFF"
                            font.pixelSize: 10
                            font.weight: Font.Bold
                            font.family: Style.fontFamily
                        }
                    }

                    Text {
                        text: "Assistant"
                        color: Style.text
                        font.pixelSize: 12
                        font.weight: Font.DemiBold
                        font.family: Style.fontFamily
                    }

                    Rectangle {
                        width: 1
                        height: 12
                        color: Style.lineStrong
                        Layout.alignment: Qt.AlignVCenter
                    }

                    Text {
                        text: "主面板"
                        color: Style.text3
                        font.pixelSize: Style.fontCaption
                        font.family: Style.fontFamily
                    }

                    Item { Layout.fillWidth: true }

                    AppIconButton {
                        icon: "minimize"
                        iconSize: 12
                        tooltip: "最小化"
                        onClicked: panelWindow.showMinimized()
                    }
                    AppIconButton {
                        icon: panelWindow.visibility === Window.Maximized ? "restore" : "maximize"
                        iconSize: 12
                        tooltip: panelWindow.visibility === Window.Maximized ? "还原" : "最大化"
                        onClicked: {
                            if (panelWindow.visibility === Window.Maximized)
                                panelWindow.showNormal()
                            else
                                panelWindow.showMaximized()
                        }
                    }
                    AppIconButton {
                        icon: "close"
                        iconSize: 12
                        danger: true
                        tooltip: "隐藏"
                        onClicked: panelWindow.hide()
                    }
                }
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 1
                color: Style.line
            }

            // ================= 主体 =================
            RowLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.margins: 10
                spacing: 10

                // ---- 左侧导航 ----
                Rectangle {
                    Layout.preferredWidth: Style.navWidth
                    Layout.fillHeight: true
                    radius: Style.radiusCard
                    color: Style.navBg
                    border.width: 1
                    border.color: Style.line

                    ColumnLayout {
                        anchors.fill: parent
                        anchors.margins: 9
                        spacing: 2

                        Repeater {
                            model: panelWindow.navModel

                            delegate: Item {
                                Layout.fillWidth: true
                                Layout.preferredHeight: modelData.type === "label"
                                                        ? 24 : 34

                                // 分组标题
                                Text {
                                    visible: modelData.type === "label"
                                    anchors.left: parent.left
                                    anchors.leftMargin: 8
                                    anchors.verticalCenter: parent.verticalCenter
                                    text: modelData.label
                                    color: Style.text3
                                    font.pixelSize: 10
                                    font.weight: Font.DemiBold
                                    font.letterSpacing: 1.2
                                    font.family: Style.fontFamily
                                }

                                // 导航项
                                Rectangle {
                                    id: navRow
                                    visible: modelData.type === "item"
                                    anchors.fill: parent
                                    radius: Style.radiusControl
                                    color: panelWindow.selectedKey === modelData.key
                                           ? Style.accentSoft
                                           : navHover.hovered ? Style.surface2 : "transparent"
                                    Behavior on color { ColorAnimation { duration: Style.motionFast } }

                                    HoverHandler { id: navHover }

                                    Rectangle {
                                        visible: panelWindow.selectedKey === modelData.key
                                        anchors.left: parent.left
                                        anchors.leftMargin: -9
                                        anchors.verticalCenter: parent.verticalCenter
                                        width: 3
                                        height: 16
                                        radius: 2
                                        color: Style.accent
                                    }

                                    RowLayout {
                                        anchors.fill: parent
                                        anchors.leftMargin: 9
                                        anchors.rightMargin: 8
                                        spacing: 9

                                        AppIcon {
                                            name: modelData.type === "item" ? modelData.icon : "file"
                                            size: 15
                                            iconColor: panelWindow.selectedKey === modelData.key
                                                       ? Style.accent : Style.text3
                                        }
                                        Text {
                                            Layout.fillWidth: true
                                            text: modelData.type === "item" ? modelData.label : ""
                                            color: panelWindow.selectedKey === modelData.key
                                                   ? Style.accent : Style.text2
                                            font.pixelSize: 13
                                            font.weight: panelWindow.selectedKey === modelData.key
                                                         ? Font.DemiBold : Font.Normal
                                            font.family: Style.fontFamily
                                            elide: Text.ElideRight
                                        }
                                    }

                                    MouseArea {
                                        anchors.fill: parent
                                        cursorShape: Qt.PointingHandCursor
                                        onClicked: panelWindow.selectedKey = modelData.key
                                    }
                                }
                            }
                        }

                        Item { Layout.fillHeight: true }

                        Rectangle {
                            Layout.fillWidth: true
                            Layout.preferredHeight: 1
                            Layout.topMargin: 4
                            Layout.bottomMargin: 4
                            color: Style.line
                        }

                        // 设置固定底部
                        Rectangle {
                            Layout.fillWidth: true
                            Layout.preferredHeight: 34
                            radius: Style.radiusControl
                            color: panelWindow.selectedKey === "settings"
                                   ? Style.accentSoft
                                   : settingsHover.hovered ? Style.surface2 : "transparent"

                            HoverHandler { id: settingsHover }

                            Rectangle {
                                visible: panelWindow.selectedKey === "settings"
                                anchors.left: parent.left
                                anchors.leftMargin: -9
                                anchors.verticalCenter: parent.verticalCenter
                                width: 3
                                height: 16
                                radius: 2
                                color: Style.accent
                            }

                            AppIcon {
                                anchors.left: parent.left
                                anchors.leftMargin: 9
                                anchors.verticalCenter: parent.verticalCenter
                                name: "settings"
                                size: 15
                                iconColor: panelWindow.selectedKey === "settings"
                                           ? Style.accent : Style.text3
                            }
                            Text {
                                anchors.left: parent.left
                                anchors.leftMargin: 33
                                anchors.verticalCenter: parent.verticalCenter
                                text: "设置"
                                color: panelWindow.selectedKey === "settings"
                                       ? Style.accent : Style.text2
                                font.pixelSize: 13
                                font.weight: panelWindow.selectedKey === "settings"
                                             ? Font.DemiBold : Font.Normal
                                font.family: Style.fontFamily
                            }

                            MouseArea {
                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor
                                onClicked: panelWindow.selectedKey = "settings"
                            }
                        }

                        RowLayout {
                            Layout.fillWidth: true
                            Layout.leftMargin: 8
                            Layout.rightMargin: 8
                            Layout.topMargin: 2
                            spacing: 6

                            Text {
                                text: "v" + appVersion
                                color: Style.text3
                                font.pixelSize: 10
                                font.family: Style.fontFamily
                            }
                            Item { Layout.fillWidth: true }
                            Text {
                                text: Style.isDark ? "深色" : "浅色"
                                color: Style.text3
                                font.pixelSize: 10
                                font.family: Style.fontFamily
                            }
                        }
                    }
                }

                // ---- 右侧页面 ----
                Item {
                    Layout.fillWidth: true
                    Layout.fillHeight: true

                    Loader {
                        id: pageLoader
                        anchors.fill: parent
                        visible: !panelWindow.isPluginKey(panelWindow.selectedKey)
                        source: panelWindow.isPluginKey(panelWindow.selectedKey)
                                ? "" : panelWindow.sourceFor(panelWindow.selectedKey)

                        onLoaded: {
                            if (item && panelWindow.selectedKey === "plugins")
                                item.api = panelWindow.pluginApi
                        }
                    }

                    Item {
                        id: pluginPageSlot
                        anchors.fill: parent
                        visible: panelWindow.isPluginKey(panelWindow.selectedKey)
                    }
                }
            }
        }
    }

    // ================= 边框/角落缩放热区 =================
    // 无边框窗口没有原生 resize 边框，用 startSystemResize 模拟；
    // 四边 + 四角，角落压在上层（z 更高）实现斜向缩放。
    readonly property bool resizable: panelWindow.visibility !== Window.Maximized

    MouseArea {
        width: 5; height: parent.height
        anchors.left: parent.left
        visible: panelWindow.resizable
        cursorShape: Qt.SizeHorCursor
        onPressed: panelWindow.startSystemResize(Qt.LeftEdge)
        z: 60
    }
    MouseArea {
        width: 5; height: parent.height
        anchors.right: parent.right
        visible: panelWindow.resizable
        cursorShape: Qt.SizeHorCursor
        onPressed: panelWindow.startSystemResize(Qt.RightEdge)
        z: 60
    }
    MouseArea {
        width: parent.width; height: 5
        anchors.top: parent.top
        visible: panelWindow.resizable
        cursorShape: Qt.SizeVerCursor
        onPressed: panelWindow.startSystemResize(Qt.TopEdge)
        z: 60
    }
    MouseArea {
        width: parent.width; height: 5
        anchors.bottom: parent.bottom
        visible: panelWindow.resizable
        cursorShape: Qt.SizeVerCursor
        onPressed: panelWindow.startSystemResize(Qt.BottomEdge)
        z: 60
    }

    // 未登录时弹出的登录窗（含离线继续）
    LoginDialog {
        id: loginDialog
    }

    // ---- 四个角：斜向缩放 ----
    MouseArea {
        width: 9; height: 9
        anchors.left: parent.left
        anchors.top: parent.top
        visible: panelWindow.resizable
        cursorShape: Qt.SizeFDiagCursor
        onPressed: panelWindow.startSystemResize(Qt.LeftEdge | Qt.TopEdge)
        z: 61
    }
    MouseArea {
        width: 9; height: 9
        anchors.right: parent.right
        anchors.top: parent.top
        visible: panelWindow.resizable
        cursorShape: Qt.SizeBDiagCursor
        onPressed: panelWindow.startSystemResize(Qt.RightEdge | Qt.TopEdge)
        z: 61
    }
    MouseArea {
        width: 9; height: 9
        anchors.left: parent.left
        anchors.bottom: parent.bottom
        visible: panelWindow.resizable
        cursorShape: Qt.SizeBDiagCursor
        onPressed: panelWindow.startSystemResize(Qt.LeftEdge | Qt.BottomEdge)
        z: 61
    }
    MouseArea {
        width: 9; height: 9
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        visible: panelWindow.resizable
        cursorShape: Qt.SizeFDiagCursor
        onPressed: panelWindow.startSystemResize(Qt.RightEdge | Qt.BottomEdge)
        z: 61
    }
}
