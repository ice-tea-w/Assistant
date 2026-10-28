import QtQuick
import QtQuick.Layouts

// 插件市场页（M0：本地已装管理；M1 起补市场目录）
//
// 分层约定（docs/architecture/插件市场-模块框架.md §2）：
//   本文件是 L1 表现层，只认 api 暴露的属性/方法/信号，
//   不碰文件系统、不发网络请求、不判断"能不能装"。
//
// api 由 MainPanel 注入，真实实现是 C++ 的 PluginBridge（context property `plugins`）。
Rectangle {
    id: page
    color: "transparent"

    // ---- 注入 ----
    property var api: null

    // ---- 本地 UI 态 ----
    property string filterText: ""
    property string category: "全部"
    property string detailId: ""
    property bool   detailOpen: false
    property bool   installOpen: false
    property string installRequestId: ""

    // ---- 主题 ----
    readonly property color textColor:  Style.textColor
    readonly property color mutedColor: Style.mutedColor
    readonly property color accentColor: Style.accentColor
    readonly property color borderColor: Style.borderColor
    readonly property color panelColor: Style.panelColor

    // ---- 派生数据 ----
    readonly property var installedRows: api ? (api.installedList || []) : []
    readonly property var availableRows: api ? (api.availableList || []) : []
    readonly property bool isOffline: api ? (api.isOffline || false) : false
    readonly property string fetchedText: api ? (api.fetchedAtText || "") : ""
    readonly property var categoryList: {
        var base = ["全部"]
        if (api && api.categories) {
            for (var i = 0; i < api.categories.length; ++i) {
                if (api.categories[i] !== "全部")
                    base.push(api.categories[i])
            }
        }
        return base
    }

    function matchText(item, t) {
        if (!t) return true
        var q = t.toLowerCase()
        return (item.name || "").toLowerCase().indexOf(q) >= 0
            || (item.summary || "").toLowerCase().indexOf(q) >= 0
            || (item.id || "").toLowerCase().indexOf(q) >= 0
    }

    function matchCategory(item, c) {
        if (c === "全部") return true
        if (!item.categories) return false
        return item.categories.indexOf(c) >= 0
    }

    /// 已安装组不受分类筛选影响（用户管理已装插件不该被分类藏起来）
    readonly property var rows: {
        var out = []
        for (var i = 0; i < installedRows.length; ++i) {
            var it = installedRows[i]
            if (!matchText(it, filterText)) continue
            out.push({ group: "已安装",
                       pluginId: it.id, name: it.name, version: it.version,
                       summary: it.summary, icon: it.icon,
                       state: it.state, badge: it.badge, badgeKind: it.badgeKind,
                       problem: it.problem, percent: it.percent || 0,
                       pluginEnabled: it.enabled !== false })
        }
        for (var j = 0; j < availableRows.length; ++j) {
            var av = availableRows[j]
            if (!matchText(av, filterText)) continue
            if (!matchCategory(av, category)) continue
            out.push({ group: "可安装",
                       pluginId: av.id, name: av.name, version: av.version,
                       summary: av.summary, icon: av.icon,
                       state: av.state, badge: av.badge, badgeKind: av.badgeKind,
                       problem: av.problem, percent: av.percent || 0,
                       pluginEnabled: av.enabled !== false })
        }
        return out
    }

    readonly property bool loading: api ? (api.loading === true) : false
    readonly property bool isEmpty: !loading && rows.length === 0 && filterText === ""

    function findRow(id) {
        for (var i = 0; i < installedRows.length; ++i)
            if (installedRows[i].id === id) return installedRows[i]
        for (var j = 0; j < availableRows.length; ++j)
            if (availableRows[j].id === id) return availableRows[j]
        return null
    }

    // ---- 页内提示（M0 先自带，M2 起可改走宿主 toast）----
    property string toastText: ""
    property bool   toastOk: true
    function showToast(ok, msg) {
        toastText = msg
        toastOk = ok
        toastTimer.restart()
    }
    Timer {
        id: toastTimer
        interval: 2600
        onTriggered: page.toastText = ""
    }

    focus: true
    Keys.onEscapePressed: {
        if (installOpen) { installOpen = false; return }
        if (detailOpen)  { detailOpen = false; return }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // ══════════ 头部 ══════════
        RowLayout {
            Layout.fillWidth: true
            Layout.preferredHeight: 44
            Layout.leftMargin: 14
            Layout.rightMargin: 14
            spacing: 8

            Text {
                text: "插件市场"
                color: page.textColor
                font.pixelSize: Style.titleSize
                font.bold: true
            }

            Item { Layout.fillWidth: true }

            // 搜索框（沿用 TodoPage 的 TextInput + 占位 Text 写法）
            Rectangle {
                Layout.preferredWidth: 168
                Layout.preferredHeight: 26
                radius: Style.radiusSmall
                color: Style.isDark ? "#262626" : "#fafafa"
                border.width: 1
                border.color: searchInput.activeFocus ? page.accentColor : page.borderColor

                TextInput {
                    id: searchInput
                    anchors.fill: parent
                    anchors.leftMargin: 8
                    anchors.rightMargin: 8
                    color: page.textColor
                    font.pixelSize: 11
                    verticalAlignment: TextInput.AlignVCenter
                    selectByMouse: true
                    onTextChanged: page.filterText = text

                    Text {
                        anchors.fill: parent
                        verticalAlignment: Text.AlignVCenter
                        visible: !searchInput.text && !searchInput.activeFocus
                        text: "搜索插件…"
                        color: page.mutedColor
                        font.pixelSize: 11
                    }
                }
            }

            // 刷新
            Rectangle {
                Layout.preferredWidth: 26
                Layout.preferredHeight: 26
                radius: Style.radiusSmall
                color: "transparent"
                border.width: 1
                border.color: refreshMouse.containsMouse ? page.accentColor : page.borderColor

                Text {
                    anchors.centerIn: parent
                    text: "↻"
                    color: refreshMouse.containsMouse ? page.accentColor : page.mutedColor
                    font.pixelSize: 15
                }

                MouseArea {
                    id: refreshMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: { if (page.api) page.api.refresh() }
                }
            }
        }

        // ══════════ 分类条 ══════════
        RowLayout {
            Layout.fillWidth: true
            Layout.preferredHeight: 32
            Layout.leftMargin: 14
            Layout.rightMargin: 14
            spacing: 6

            Repeater {
                model: page.categoryList

                Rectangle {
                    height: 20
                    width: chipText.implicitWidth + 18
                    radius: 10
                    color: page.category === modelData ? page.accentColor
                                                       : (Style.isDark ? "#262626" : "#f5f5f5")

                    Text {
                        id: chipText
                        anchors.centerIn: parent
                        text: modelData
                        font.pixelSize: 11
                        color: page.category === modelData ? Style.accentTextColor
                                                           : page.mutedColor
                    }

                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: page.category = modelData
                    }
                }
            }

            Item { Layout.fillWidth: true }
        }

        // 细分隔线
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 1
            color: page.borderColor
        }

        // ══════════ 列表 ══════════
        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true

            ListView {
                id: pluginList
                anchors.fill: parent
                anchors.leftMargin: 14
                anchors.rightMargin: 14
                anchors.topMargin: 4
                anchors.bottomMargin: 4
                clip: true
                spacing: 6
                model: page.rows
                visible: !page.loading && page.rows.length > 0

                // 分组标题（已安装 / 可安装）
                section.property: "group"
                section.delegate: Item {
                    width: ListView.view.width
                    height: 22

                    Text {
                        anchors.left: parent.left
                        anchors.bottom: parent.bottom
                        anchors.bottomMargin: 3
                        text: section
                        color: page.mutedColor
                        font.pixelSize: 11
                    }
                }

                delegate: PluginCard {
                    width: ListView.view.width

                    pluginId:      modelData.pluginId
                    name:          modelData.name
                    version:       modelData.version
                    summary:       modelData.summary
                    icon:          modelData.icon
                    state:         modelData.state
                    badge:         modelData.badge
                    badgeKind:     modelData.badgeKind
                    problem:       modelData.problem
                    percent:       modelData.percent
                    pluginEnabled: modelData.pluginEnabled

                    onInstallClicked: {
                        if (page.api) page.api.install(pluginId)
                        page.installRequestId = pluginId
                        page.installOpen = true
                    }
                    onCancelClicked: {
                        if (page.api) page.api.cancelInstall(pluginId)
                    }
                    onToggleToggled: function(want) {
                        if (page.api) page.api.setEnabled(pluginId, want)
                    }
                    onUninstallClicked: {
                        page.detailId = pluginId
                        page.detailOpen = true
                    }
                    onDetailClicked: {
                        page.detailId = pluginId
                        page.detailOpen = true
                    }
                    onUpdateClicked: {
                        if (page.api) page.api.install(pluginId)
                    }
                }
            }

            // ---- 骨架屏（首次加载）----
            Column {
                anchors.fill: parent
                anchors.leftMargin: 14
                anchors.rightMargin: 14
                anchors.topMargin: 8
                spacing: 6
                visible: page.loading

                Repeater {
                    model: 4
                    Rectangle {
                        width: parent.width
                        height: 64
                        radius: Style.radius
                        color: Style.isDark ? "#262626" : "#f2f2f2"
                    }
                }
            }

            // ---- 空态 ----
            ColumnLayout {
                anchors.centerIn: parent
                spacing: 8
                visible: page.isEmpty

                Text {
                    Layout.alignment: Qt.AlignHCenter
                    text: "🧩"
                    font.pixelSize: 32
                    opacity: 0.5
                }
                Text {
                    Layout.alignment: Qt.AlignHCenter
                    text: filterText !== "" ? "没有匹配的插件"
                                            : "市场还没有内容"
                    color: page.mutedColor
                    font.pixelSize: 13
                }
                Text {
                    Layout.alignment: Qt.AlignHCenter
                    visible: filterText === ""
                    text: "把插件文件夹放进 plugins 目录即可加载"
                    color: page.mutedColor
                    font.pixelSize: 11
                    opacity: 0.8
                }
            }
        }

        // ══════════ 底部离线提示 ══════════
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: visible ? 24 : 0
            visible: page.isOffline && fetchedText !== ""
            color: Style.isDark ? "#332b1a" : "#fdf8ec"

            Text {
                anchors.fill: parent
                anchors.leftMargin: 14
                anchors.rightMargin: 14
                verticalAlignment: Text.AlignVCenter
                elide: Text.ElideRight
                text: "离线：显示的是 " + page.fetchedText + " 的缓存内容"
                color: Style.isDark ? "#d8b878" : "#9a6b1e"
                font.pixelSize: 10
            }
        }
    }

    // ══════════ 详情面板（右侧滑出）══════════
    Item {
        anchors.fill: parent
        visible: detailOpen
        z: 10

        // 遮罩
        Rectangle {
            anchors.fill: parent
            color: "#000000"
            opacity: detailOpen ? 0.18 : 0
            Behavior on opacity { NumberAnimation { duration: 160 } }

            MouseArea {
                anchors.fill: parent
                onClicked: page.detailOpen = false
            }
        }

        Rectangle {
            id: detailPanel
            width: 340
            height: parent.height
            anchors.right: parent.right
            x: detailOpen ? parent.width - width : parent.width
            color: page.panelColor
            border.width: 1
            border.color: page.borderColor
            Behavior on x { NumberAnimation { duration: 180; easing.type: Easing.OutCubic } }

            property var row: page.detailId !== "" ? page.findRow(page.detailId) : null

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 16
                spacing: 10

                // 头部
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 10

                    Text {
                        text: detailPanel.row && detailPanel.row.problem ? "⚠"
                              : (detailPanel.row && detailPanel.row.icon ? detailPanel.row.icon : "🧩")
                        font.pixelSize: 28
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 2

                        Text {
                            text: detailPanel.row ? detailPanel.row.name : ""
                            color: page.textColor
                            font.pixelSize: 15
                            font.bold: true
                        }
                        Text {
                            text: detailPanel.row
                                  ? ("v" + detailPanel.row.version
                                     + (detailPanel.row.badge ? " · " + detailPanel.row.badge : ""))
                                  : ""
                            color: page.mutedColor
                            font.pixelSize: 11
                        }
                    }

                    Text {
                        text: "✕"
                        color: closeMouse.containsMouse ? page.textColor : page.mutedColor
                        font.pixelSize: 14

                        MouseArea {
                            id: closeMouse
                            anchors.fill: parent
                            anchors.margins: -6
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: page.detailOpen = false
                        }
                    }
                }

                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 1
                    color: page.borderColor
                }

                // 问题详情（有问题时优先显示）
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 4
                    visible: detailPanel.row && detailPanel.row.problem

                    Text {
                        text: "问题"
                        color: page.mutedColor
                        font.pixelSize: 11
                    }
                    Text {
                        Layout.fillWidth: true
                        text: detailPanel.row ? (detailPanel.row.problem || "") : ""
                        color: Style.dangerColor
                        font.pixelSize: 12
                        wrapMode: Text.Wrap
                    }
                }

                // 描述
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 4

                    Text {
                        text: "描述"
                        color: page.mutedColor
                        font.pixelSize: 11
                    }
                    Text {
                        Layout.fillWidth: true
                        text: detailPanel.row ? (detailPanel.row.summary || "—") : ""
                        color: page.textColor
                        font.pixelSize: 12
                        wrapMode: Text.Wrap
                    }
                }

                // 权限（M0 仅展示）
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 4
                    visible: detailPanel.row && detailPanel.row.permissions
                             && detailPanel.row.permissions.length > 0

                    Text {
                        text: "权限"
                        color: page.mutedColor
                        font.pixelSize: 11
                    }
                    Repeater {
                        model: detailPanel.row && detailPanel.row.permissions
                               ? detailPanel.row.permissions : []
                        RowLayout {
                            spacing: 6
                            Text {
                                text: "·"
                                color: page.mutedColor
                                font.pixelSize: 12
                            }
                            Text {
                                text: page.permissionLabel(modelData)
                                color: page.textColor
                                font.pixelSize: 12
                            }
                        }
                    }
                }

                Item { Layout.fillHeight: true }

                // 底部动作
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8

                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 30
                        radius: Style.radiusSmall
                        color: "transparent"
                        border.width: 1
                        border.color: page.borderColor

                        Text {
                            anchors.centerIn: parent
                            text: "打开目录"
                            color: page.textColor
                            font.pixelSize: 12
                        }

                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: {
                                if (page.api) page.api.openPluginDir(page.detailId)
                            }
                        }
                    }

                    Rectangle {
                        Layout.preferredWidth: 78
                        Layout.preferredHeight: 30
                        radius: Style.radiusSmall
                        color: Style.dangerColor
                        visible: detailPanel.row && detailPanel.row.state !== "bundled"

                        Text {
                            anchors.centerIn: parent
                            text: "卸载"
                            color: "#ffffff"
                            font.pixelSize: 12
                        }

                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: {
                                if (page.api) page.api.uninstall(page.detailId)
                                page.detailOpen = false
                                page.showToast(true, "已卸载 " + (detailPanel.row ? detailPanel.row.name : ""))
                            }
                        }
                    }
                }

                Text {
                    Layout.fillWidth: true
                    visible: detailPanel.row && detailPanel.row.state === "bundled"
                    text: "内置插件不可卸载，可关闭开关停用。"
                    color: page.mutedColor
                    font.pixelSize: 10
                    wrapMode: Text.Wrap
                }
            }
        }
    }

    // ══════════ 安装对话框 ══════════
    Item {
        anchors.fill: parent
        visible: installOpen
        z: 20

        Rectangle {
            anchors.fill: parent
            color: "#000000"
            opacity: 0.25

            MouseArea {
                anchors.fill: parent
                onClicked: page.installOpen = false
            }
        }

        Rectangle {
            width: 380
            height: 240
            anchors.centerIn: parent
            radius: Style.radius
            color: page.panelColor
            border.width: 1
            border.color: page.borderColor

            property var row: page.installRequestId !== "" ? page.findRow(page.installRequestId) : null

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 16
                spacing: 10

                Text {
                    text: installPanel.row ? ("安装 " + installPanel.row.name
                                             + "  v" + installPanel.row.version)
                                           : "安装插件"
                    color: page.textColor
                    font.pixelSize: 14
                    font.bold: true
                }

                // 信任提示（常驻不折叠，LLD §9.4）
                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 56
                    radius: Style.radiusSmall
                    color: Style.isDark ? "#332b1a" : "#fdf8ec"

                    Text {
                        anchors.fill: parent
                        anchors.margins: 8
                        text: "插件以你的权限在本程序内运行，可读写你的文件。"
                              + "只安装你信任的来源。已安装的插件不会自动更新。"
                        color: Style.isDark ? "#d8b878" : "#9a6b1e"
                        font.pixelSize: 10
                        wrapMode: Text.Wrap
                    }
                }

                // 阶段清单（M0 只有占位；M2 接真实进度）
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 4

                    Repeater {
                        model: ["解析清单", "下载", "解压", "校验清单", "提交"]
                        RowLayout {
                            spacing: 8
                            Text {
                                text: "○"
                                color: page.mutedColor
                                font.pixelSize: 11
                            }
                            Text {
                                text: modelData
                                color: page.mutedColor
                                font.pixelSize: 11
                            }
                        }
                    }
                }

                Item { Layout.fillHeight: true }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8
                    Item { Layout.fillWidth: true }

                    Rectangle {
                        Layout.preferredWidth: 70
                        Layout.preferredHeight: 28
                        radius: Style.radiusSmall
                        color: "transparent"
                        border.width: 1
                        border.color: page.borderColor

                        Text {
                            anchors.centerIn: parent
                            text: "取消"
                            color: page.textColor
                            font.pixelSize: 12
                        }

                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: page.installOpen = false
                        }
                    }

                    Rectangle {
                        Layout.preferredWidth: 70
                        Layout.preferredHeight: 28
                        radius: Style.radiusSmall
                        color: page.accentColor

                        Text {
                            anchors.centerIn: parent
                            text: "开始安装"
                            color: Style.accentTextColor
                            font.pixelSize: 12
                        }

                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: {
                                if (page.api) page.api.install(page.installRequestId)
                                page.installOpen = false
                                page.showToast(true, "已发起安装")
                            }
                        }
                    }
                }
            }
        }
    }

    // ══════════ 页内 toast ══════════
    Rectangle {
        id: toast
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 34
        height: 30
        width: toastLabel.implicitWidth + 28
        radius: Style.radiusSmall
        color: page.toastOk ? (Style.isDark ? "#2a2a2a" : "#333333") : Style.dangerColor
        opacity: page.toastText !== "" ? 1 : 0
        visible: opacity > 0
        Behavior on opacity { NumberAnimation { duration: 150 } }

        Text {
            id: toastLabel
            anchors.centerIn: parent
            text: page.toastText
            color: "#ffffff"
            font.pixelSize: 11
        }
    }

    /// 权限点 → 人类可读（LLD §9.4）
    function permissionLabel(p) {
        switch (p) {
        case "network":         return "访问网络"
        case "fs.read":         return "读取你的文件"
        case "fs.write":        return "写入你的文件"
        case "clipboard.read":  return "读取剪贴板"
        case "clipboard.write": return "写入剪贴板"
        case "hotkey":          return "注册全局热键"
        case "shell":           return "运行外部程序"
        default:                return p
        }
    }

    Connections {
        target: page.api
        ignoreUnknownSignals: true

        function onToast(level, text) {
            page.showToast(level !== "error", text)
        }
    }
}
