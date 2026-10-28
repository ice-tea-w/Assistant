import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Phase 3 主面板 —— 常驻窗口（区别于搜索浮窗）：
// 不置顶、非 Tool、失焦不隐藏、可最小化。
//
// 左侧功能栏：待办 / 记事本 / 剪贴板 / 插件市场（从上到下）+ 设置（固定底部）。
// 右侧用 Loader 切换页面。
//
// ── 为什么从 StackLayout 改成 Loader（插件市场 M0）──
// 插件会往导航里加页面，数量是运行时才知道的；StackLayout 的子项模型是
// 静态声明的，塞不进动态页面。Loader + source 能按 key 计算目标页。
//
// ── 为什么从 selectedIndex 改成 selectedKey ──
// 原来用索引对齐（`selectedIndex === 3` 表示设置）在 4 个固定页时能用，
// 但加一项之后设置页的索引就变了，而且插件页插在中间会让所有索引错位。
// 用 key 后，加/减页面只影响 navItems 数组，不影响任何判定。
ApplicationWindow {
    id: panelWindow
    width: 760
    height: 520
    title: "Assistant"
    color: "transparent"
    visible: false   // 由 🏠 按钮 / 托盘菜单触发显示

    // ---- Theme colors：统一来自 Style 单例（同目录 qmldir 的 singleton）----
    readonly property bool isDark: Style.isDark
    readonly property color bgColor:     Style.bgColor
    readonly property color panelColor:  Style.panelColor
    readonly property color textColor:   Style.textColor
    readonly property color mutedColor:  Style.mutedColor
    readonly property color accentColor: Style.accentColor
    readonly property color borderColor: Style.borderColor

    // ---- 导航状态：以 key 为准，不以索引对齐 ----
    property string selectedKey: "todo"

    // ---- 插件市场数据源 ----
    // 由 main.cpp 注入的 PluginBridge（context property `plugins`）。
    // 保留 typeof 判断是为了让本文件能被单独加载（qmllint / 预览）而不报错；
    // 正式运行时 `plugins` 一定存在。
    readonly property var pluginApi: (typeof plugins !== "undefined") ? plugins : null

    // 内置导航项（从上到下）；设置项单独固定在底部。
    readonly property var builtinNav: [
        { "key": "todo",    "icon": "✓",  "label": "待办事项" },
        { "key": "notes",   "icon": "📝", "label": "记事本"   },
        { "key": "clip",    "icon": "📋", "label": "剪贴板"   },
        { "key": "plugins", "icon": "🧩", "label": "插件市场" }
    ]

    // 插件贡献的页面 → 统一成导航项的形状。
    // key 形如 "com.assistant.demo:demo"；依赖 pluginApi.navigationEntries，
    // 后者在 loaded / unloaded 时通过 navigationChanged 通知。
    readonly property var pluginNav: {
        var out = []
        if (!pluginApi || !pluginApi.navigationEntries) return out
        var entries = pluginApi.navigationEntries
        for (var i = 0; i < entries.length; ++i) {
            var e = entries[i]
            out.push({ "key": e.key, "icon": e.icon ? e.icon : "🧩", "label": e.title })
        }
        return out
    }

    readonly property var navItems: builtinNav.concat(pluginNav)

    // 插件页面的 key 一定带冒号；内置 key 都不带。
    function isPluginKey(key) {
        return typeof key === "string" && key.indexOf(":") > 0
    }

    function indexOfKey(key) {
        for (var i = 0; i < navItems.length; ++i)
            if (navItems[i].key === key) return i
        return -1
    }

    function sourceFor(key) {
        switch (key) {
        case "todo":    return "qrc:/qml/TodoPage.qml"
        case "notes":   return "qrc:/qml/NotesPage.qml"
        case "clip":    return "qrc:/qml/ClipboardPage.qml"
        case "plugins": return "qrc:/qml/PluginsPage.qml"
        case "settings":return "qrc:/qml/SettingsPage.qml"
        }
        return ""   // 插件页由 C++ 创建，不走 Loader
    }

    // ---- 插件贡献的页面 ----
    // 这些页面里会用到 host.xxx，而 host 只存在于插件自己的 QQmlContext。
    // 所以不能用 Loader + file:// 加载（那会跑在面板 engine 的根上下文里），
    // 必须由 C++ 用插件的上下文创建，这里只负责把对象挂进来并铺满。
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

        // 必须同时满足两件事：key 是插件页、且它确实在导航里。
        // 只看 isPluginKey 不够：Component.onCompleted 早于 loadEnabledAtStartup()，
        // 那时 navigationEntries 还是空的，直接去 acquirePage 会白报一次错。
        if (!isPluginKey(selectedKey) || indexOfKey(selectedKey) < 0) {
            pluginPageItem = null
            return
        }

        var obj = pluginApi ? pluginApi.acquirePage(selectedKey) : null
        pluginPageItem = obj
        if (!obj) return

        obj.parent = pluginPageSlot          // QQuickItem 的视觉父项
        obj.visible = true
        obj.width = Qt.binding(function() { return pluginPageSlot.width })
        obj.height = Qt.binding(function() { return pluginPageSlot.height })
    }

    onSelectedKeyChanged: attachPluginPage()

    // 正在查看的插件页被卸载/删除了 → 退回插件市场，避免停在空页面上。
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

    // 点 X 只隐藏，不销毁（主面板常驻，下次 🏠 重新 show）
    onClosing: function(close) {
        close.accepted = false
        panelWindow.hide()
    }

    Rectangle {
        anchors.fill: parent
        radius: 12
        color: bgColor
        border.color: borderColor
        border.width: 1

        RowLayout {
            anchors.fill: parent
            anchors.margins: 10
            spacing: 10

            // ---- 左侧导航栏 ----
            Rectangle {
                Layout.preferredWidth: 150
                Layout.fillHeight: true
                radius: 8
                color: panelColor

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 8
                    spacing: 4

                    // 功能入口（从上到下）
                    ListView {
                        id: navList
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        clip: true
                        model: panelWindow.navItems
                        currentIndex: panelWindow.indexOfKey(panelWindow.selectedKey)
                        spacing: 4

                        delegate: Rectangle {
                            width: ListView.view.width
                            height: 40
                            radius: 6
                            color: index === navList.currentIndex
                                   ? accentColor : "transparent"

                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: 12
                                anchors.rightMargin: 12
                                spacing: 8

                                Text {
                                    text: modelData.icon
                                    font.pixelSize: 14
                                }
                                Text {
                                    text: modelData.label
                                    color: index === navList.currentIndex
                                           ? Style.accentTextColor : textColor
                                    font.pixelSize: 14
                                    Layout.fillWidth: true
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

                    // 分隔线
                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 1
                        color: borderColor
                    }

                    // ---- 设置（固定底部）----
                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 40
                        radius: 6
                        color: panelWindow.selectedKey === "settings"
                               ? accentColor : "transparent"

                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: 12
                            anchors.rightMargin: 12
                            spacing: 8

                            Text { text: "⚙"; font.pixelSize: 14 }
                            Text {
                                text: "设置"
                                color: panelWindow.selectedKey === "settings"
                                       ? Style.accentTextColor : textColor
                                font.pixelSize: 14
                            }
                        }

                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: panelWindow.selectedKey = "settings"
                        }
                    }
                }
            }

            // ---- 右侧功能区 ----
            Item {
                Layout.fillWidth: true
                Layout.fillHeight: true

                // 内置页面
                Loader {
                    id: pageLoader
                    anchors.fill: parent
                    visible: !panelWindow.isPluginKey(panelWindow.selectedKey)
                    source: panelWindow.isPluginKey(panelWindow.selectedKey)
                            ? "" : panelWindow.sourceFor(panelWindow.selectedKey)

                    // 插件市场页需要一个 api 注入点（其余页面走全局的 `panel`）
                    onLoaded: {
                        if (item && panelWindow.selectedKey === "plugins")
                            item.api = panelWindow.pluginApi
                    }
                }

                // 插件贡献的页面：对象由 C++ 创建，这里只提供挂载位
                Item {
                    id: pluginPageSlot
                    anchors.fill: parent
                    visible: panelWindow.isPluginKey(panelWindow.selectedKey)
                }
            }
        }
    }
}
