import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "components"

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

    // ---- 安装前确认（信任提示）----
    // 为什么要有它：插件在宿主机进程内、以用户权限运行，`permissions` 只是声明、
    // 宿主**不会**据此拦截（见 docs/plugins/README.md §9）。不给这句提示，
    // UI 会让人误以为有沙箱。
    property bool   confirmOpen: false
    property string confirmId: ""
    property bool   confirmIsUpdate: false

    function confirmRow() {
        return page.findRow(page.confirmId)
    }

    function requestInstall(pluginId) {
        var row = page.findRow(pluginId)
        if (!row)
            return
        page.confirmId     = pluginId
        page.confirmIsUpdate = (row.state === "update-available")
        page.confirmOpen   = true
    }

    function doInstall() {
        var id = page.confirmId
        page.confirmOpen = false
        if (id !== "" && page.api)
            page.api.install(id)
    }

    // 能力名 → 人话。声明是给人看的，别直接把 fs.write 丢到界面上。
    function permissionText(name) {
        switch (name) {
            case "network":         return "访问网络"
            case "fs.read":         return "读取你的文件"
            case "fs.write":        return "写入你的文件"
            case "clipboard.read":  return "读取剪贴板"
            case "clipboard.write": return "写入剪贴板"
            case "hotkey":          return "注册全局热键"
            case "shell":           return "打开外部程序或链接"
            default:                return name
        }
    }

    function confirmVersionText() {
        var r = page.confirmRow()
        if (!r)
            return ""
        if (page.confirmIsUpdate && r.latestVersion)
            return "v" + r.version + "  →  v" + r.latestVersion
        return "v" + r.version
    }

    function confirmPermissions() {
        var r = page.confirmRow()
        return (r && r.permissions) ? r.permissions : []
    }

    // ---- 安装进度（完全由 PluginInstaller 的信号驱动，QML 不猜进度）----
    property bool   installVisible: false
    property bool   installFinished: false
    property bool   installOk: true
    property int    installPercent: 0
    property int    installStage: 0
    property string installDetail: ""
    property string installRequestId: ""
    property string installMessage: ""
    property var    installLog: []

    function beginInstall(requestId) {
        installRequestId = requestId
        installFinished   = false
        installOk         = true
        installPercent    = 0
        installStage      = 0
        installDetail     = "准备中"
        installMessage    = ""
        installLog        = []
        installVisible    = true
    }

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
                       demo: it.demo === true,
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
                       demo: av.demo === true,
                       pluginEnabled: av.enabled !== false })
        }
        return out
    }

    readonly property bool loading: api ? (api.loading === true) : false

    /// 可安装组里是否有内置示例数据（决定分组标题要不要加说明）
    readonly property bool marketIsDemo: {
        var list = availableRows
        for (var i = 0; i < list.length; ++i)
            if (list[i].demo === true) return true
        return false
    }
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
        if (confirmOpen) { confirmOpen = false; return }
        if (installVisible) { installVisible = false; return }
        if (detailOpen)  { detailOpen = false; return }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // ══════════ 头部 ══════════
        Item {
            Layout.fillWidth: true
            Layout.preferredHeight: 64
            Layout.topMargin: 16
            Layout.leftMargin: 20
            Layout.rightMargin: 20

            RowLayout {
                anchors.fill: parent
                spacing: 8

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 2

                Text {
                    text: "插件市场"
                    color: Style.text
                    font.pixelSize: Style.fontTitle
                    font.weight: Font.DemiBold
                    font.family: Style.fontFamily
                }
                Text {
                    text: page.installedRows.length + " 已安装 · "
                          + page.availableRows.length + " 可安装"
                    color: Style.text3
                    font.pixelSize: 10
                    font.family: Style.fontFamily
                }
            }

            AppButton {
                text: "从文件安装"
                icon: "plus"
                variant: "primary"
                small: true
                onClicked: { if (page.api) page.api.pickAndInstall() }
            }

            AppIconButton {
                icon: "refresh"
                iconSize: 14
                tooltip: "刷新市场"
                onClicked: { if (page.api) page.api.refresh() }
            }
            }
        }

        // ══════════ 搜索 + 分类 ══════════
        ColumnLayout {
            Layout.fillWidth: true
            Layout.leftMargin: 20
            Layout.rightMargin: 20
            Layout.topMargin: 10
            spacing: 8

            AppTextField {
                id: searchInput
                Layout.fillWidth: true
                compact: true
                placeholder: "搜索插件名称、描述或 id…"
                leadingIcon: "search"
                clearable: true
                onEdited: page.filterText = text
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 4

                Repeater {
                    model: page.categoryList

                    delegate: AppChip {
                        text: modelData
                        selected: page.category === modelData
                        onClicked: page.category = modelData
                    }
                }

                Item { Layout.fillWidth: true }
            }
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
                anchors.leftMargin: 20
                anchors.rightMargin: 20
                anchors.topMargin: 6
                anchors.bottomMargin: 4
                clip: true
                spacing: 8
                model: page.rows
                visible: !page.loading && page.rows.length > 0
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: AppScrollBar { }

                // 分组标题（已安装 / 可安装）
                section.property: "group"
                section.delegate: Item {
                    width: ListView.view.width
                    height: 28

                    RowLayout {
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.bottom: parent.bottom
                        anchors.bottomMargin: 4
                        spacing: 6

                        Text {
                            text: section
                            color: Style.text3
                            font.pixelSize: 10
                            font.weight: Font.DemiBold
                            font.letterSpacing: 1.0
                            font.family: Style.fontFamily
                        }

                        Text {
                            text: "示例数据 · 安装功能尚未接入真实市场"
                            color: Style.text3
                            font.pixelSize: 10
                            font.family: Style.fontFamily
                            opacity: 0.8
                            visible: section === "可安装" && page.marketIsDemo
                        }

                        Item { Layout.fillWidth: true }
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
                    demo:          modelData.demo
                    pluginEnabled: modelData.pluginEnabled

                    onInstallClicked: {
                        // 先弹确认（信任提示 + 权限清单），确认后才真的开始安装
                        page.requestInstall(pluginId)
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
                        // 更新 = 装新版覆盖（同一个安装器路径），确认框里说明"会保留设置"
                        page.requestInstall(pluginId)
                    }
                }
            }

            // ---- 骨架屏（首次加载）----
            Column {
                anchors.fill: parent
                anchors.leftMargin: 20
                anchors.rightMargin: 20
                anchors.topMargin: 8
                spacing: 8
                visible: page.loading

                Repeater {
                    model: 4
                    Rectangle {
                        width: parent.width
                        height: 64
                        radius: Style.radiusCard
                        color: Style.surface2
                        border.width: 1
                        border.color: Style.line
                    }
                }
            }

            // ---- 空态 ----
            AppEmptyState {
                anchors.centerIn: parent
                width: 360
                height: 190
                visible: page.isEmpty
                icon: "plugin"
                title: page.filterText !== "" ? "没有匹配的插件" : "市场还没有内容"
                hint: page.filterText !== ""
                      ? "换个关键词，或切到「全部」分类看看"
                      : "把插件文件夹放进 plugins 目录即可加载；也可以从文件安装。"
            }
        }

        // ══════════ 底部离线提示 ══════════
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: visible ? 28 : 0
            Layout.leftMargin: 20
            Layout.rightMargin: 20
            Layout.bottomMargin: visible ? 12 : 0
            radius: Style.radiusControl
            visible: page.isOffline && fetchedText !== ""
            color: Style.alpha(Style.warning, 0.12)

            Text {
                anchors.fill: parent
                anchors.leftMargin: 10
                anchors.rightMargin: 10
                verticalAlignment: Text.AlignVCenter
                elide: Text.ElideRight
                text: "离线：显示的是 " + page.fetchedText + " 的缓存内容"
                color: Style.warning
                font.pixelSize: 10
                font.family: Style.fontFamily
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

                // 插件贡献的命令（`commands` 扩展点的消费者）
                // 这是"扩展点真的有人读"的证据：注册了就会出现，点得动。
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 4
                    visible: page.commandsOf(page.detailId).length > 0

                    Text {
                        text: "提供的命令"
                        color: page.mutedColor
                        font.pixelSize: 11
                    }

                    Repeater {
                        model: page.commandsOf(page.detailId)

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 8

                            Text {
                                Layout.fillWidth: true
                                text: modelData.title
                                color: page.textColor
                                font.pixelSize: 12
                                elide: Text.ElideRight
                            }

                            Text {
                                text: modelData.keyword ? ("/" + modelData.keyword) : ""
                                color: page.mutedColor
                                font.pixelSize: 10
                            }

                            Rectangle {
                                Layout.preferredWidth: 48
                                Layout.preferredHeight: 24
                                radius: Style.radiusSmall
                                color: runMouse.containsMouse ? page.accentColor : "transparent"
                                border.width: 1
                                border.color: page.borderColor

                                Text {
                                    anchors.centerIn: parent
                                    text: "运行"
                                    color: runMouse.containsMouse ? "#ffffff" : page.textColor
                                    font.pixelSize: 11
                                }

                                MouseArea {
                                    id: runMouse
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: {
                                        if (page.api)
                                            page.api.runCommand(modelData.pluginId, modelData.id)
                                    }
                                }
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

    // ══════════ 安装确认（信任提示）══════════
    // 这一步存在的唯一理由：把"插件以你的权限运行、宿主不拦截"说清楚。
    // 不做这一步，界面看起来就像有沙箱。
    Item {
        id: confirmOverlay
        anchors.fill: parent
        visible: page.confirmOpen
        z: 19

        Rectangle {
            anchors.fill: parent
            color: "#000000"
            opacity: 0.25

            MouseArea {
                anchors.fill: parent
                onClicked: page.confirmOpen = false
            }
        }

        Rectangle {
            width: 420
            anchors.centerIn: parent
            // 高度由内容撑开；但 ColumnLayout **不能** anchors.fill 父矩形，
            // 否则 parent.height ↔ layout.implicitHeight 互相绑定（binding loop）。
            height: confirmColumn.implicitHeight + 32
            radius: Style.radius
            color: page.panelColor
            border.width: 1
            border.color: page.borderColor

            ColumnLayout {
                id: confirmColumn
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.margins: 16
                spacing: 10

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 10

                    Text {
                        text: page.confirmRow() && page.confirmRow().icon
                              ? page.confirmRow().icon : "🧩"
                        font.pixelSize: 22
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 2

                        Text {
                            text: (page.confirmIsUpdate ? "更新 " : "安装 ")
                                  + (page.confirmRow() ? page.confirmRow().name : "")
                            color: page.textColor
                            font.pixelSize: 14
                            font.bold: true
                        }

                        Text {
                            text: page.confirmVersionText()
                            color: page.mutedColor
                            font.pixelSize: 11
                        }
                    }

                    Item {
                        width: 16; height: 16
                        Text {
                            anchors.centerIn: parent
                            text: "✕"
                            color: page.mutedColor
                            font.pixelSize: 14
                        }
                        MouseArea {
                            anchors.fill: parent
                            anchors.margins: -6
                            cursorShape: Qt.PointingHandCursor
                            onClicked: page.confirmOpen = false
                        }
                    }
                }

                // ---- 权限声明 ----
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 3
                    visible: page.confirmPermissions().length > 0

                    Text {
                        text: "这个插件声明需要："
                        color: page.mutedColor
                        font.pixelSize: 11
                    }

                    Repeater {
                        model: page.confirmPermissions()

                        Text {
                            text: "• " + page.permissionText(modelData)
                            color: page.textColor
                            font.pixelSize: 11
                        }
                    }
                }

                // ---- 信任提示（常驻，不可折叠）----
                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: trustText.implicitHeight + 16
                    radius: Style.radiusSmall
                    color: Style.isDark ? "#2a2422" : "#fff8f0"
                    border.width: 1
                    border.color: Style.isDark ? "#4a3b32" : "#f0d9bd"

                    Text {
                        id: trustText
                        anchors.fill: parent
                        anchors.margins: 8
                        text: "插件会以你的权限在本程序内运行，能读写你的文件、访问网络。"
                              + "宿主只校验包完整性，不限制插件行为（当前版本没有沙箱）。\n"
                              + "只安装你信任的来源。已安装的插件不会自动更新。"
                        color: page.textColor
                        font.pixelSize: 11
                        wrapMode: Text.Wrap
                        lineHeight: 1.25
                    }
                }

                Text {
                    Layout.fillWidth: true
                    visible: page.confirmIsUpdate
                    text: "更新会保留插件的数据目录（设置不会丢），但需要重启 Assistant 才生效。"
                    color: page.mutedColor
                    font.pixelSize: 10
                    wrapMode: Text.Wrap
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8

                    Item { Layout.fillWidth: true }

                    Rectangle {
                        Layout.preferredWidth: 64
                        Layout.preferredHeight: 30
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
                            onClicked: page.confirmOpen = false
                        }
                    }

                    Rectangle {
                        Layout.preferredWidth: 84
                        Layout.preferredHeight: 30
                        radius: Style.radiusSmall
                        color: Style.accentColor

                        Text {
                            anchors.centerIn: parent
                            text: page.confirmIsUpdate ? "更新" : "安装"
                            color: Style.accentTextColor
                            font.pixelSize: 12
                        }

                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: page.doInstall()
                        }
                    }
                }
            }
        }
    }

    // ══════════ 安装进度对话框 ══════════
    // 显示什么完全由 PluginInstaller 的 progress / log / finished 信号决定。
    Item {
        id: installOverlay
        anchors.fill: parent
        visible: page.installVisible
        z: 20

        Rectangle {
            anchors.fill: parent
            color: "#000000"
            opacity: 0.25

            MouseArea {
                anchors.fill: parent
                onClicked: page.installVisible = false
            }
        }

        Rectangle {
            width: 400
            height: 300
            anchors.centerIn: parent
            radius: Style.radius
            color: page.panelColor
            border.width: 1
            border.color: page.borderColor

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 16
                spacing: 10

                RowLayout {
                    Layout.fillWidth: true

                    Text {
                        Layout.fillWidth: true
                        text: page.installFinished
                              ? (page.installOk ? "安装完成" : "安装失败")
                              : "正在安装插件"
                        color: page.installFinished && !page.installOk
                               ? Style.dangerColor : page.textColor
                        font.pixelSize: 14
                        font.bold: true
                    }

                    Item {
                        width: 16; height: 16
                        Text {
                            anchors.centerIn: parent
                            text: "✕"
                            color: closeInstallMouse.containsMouse
                                   ? page.textColor : page.mutedColor
                            font.pixelSize: 14
                        }
                        MouseArea {
                            id: closeInstallMouse
                            anchors.fill: parent
                            anchors.margins: -6
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: page.installVisible = false
                        }
                    }
                }

                // 进度条
                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 6
                    radius: 3
                    visible: !page.installFinished
                    color: Style.isDark ? "#3a3a3a" : "#e6e6e6"

                    Rectangle {
                        width: parent.width * Math.max(0, Math.min(1, page.installPercent / 100))
                        height: parent.height
                        radius: 3
                        color: Style.accentColor
                        Behavior on width { NumberAnimation { duration: 180 } }
                    }
                }

                Text {
                    Layout.fillWidth: true
                    text: page.installFinished
                          ? (page.installOk
                             ? (page.installMessage !== "" ? page.installMessage
                                                           : "插件已写入并启用")
                             : page.installMessage)
                          : page.installDetail
                    color: page.installFinished && !page.installOk
                           ? Style.dangerColor : page.mutedColor
                    font.pixelSize: 11
                    wrapMode: Text.Wrap
                }

                // 安装日志
                Rectangle {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    radius: Style.radiusSmall
                    color: Style.isDark ? "#242424" : "#fafafa"
                    border.width: 1
                    border.color: page.borderColor

                    ListView {
                        id: installLogList
                        anchors.fill: parent
                        anchors.margins: 8
                        clip: true
                        model: page.installLog
                        spacing: 2
                        onCountChanged: positionViewAtEnd()

                        delegate: Text {
                            width: ListView.view.width
                            text: modelData
                            color: page.mutedColor
                            font.pixelSize: 10
                            font.family: "Consolas"
                            wrapMode: Text.Wrap
                        }
                    }

                    Text {
                        anchors.centerIn: parent
                        visible: installLogList.count === 0
                        text: "等待开始…"
                        color: page.mutedColor
                        font.pixelSize: 10
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8
                    Item { Layout.fillWidth: true }

                    Rectangle {
                        Layout.preferredWidth: 88
                        Layout.preferredHeight: 28
                        radius: Style.radiusSmall
                        color: "transparent"
                        border.width: 1
                        border.color: page.borderColor

                        Text {
                            anchors.centerIn: parent
                            text: page.installFinished ? "关闭" : "取消安装"
                            color: page.textColor
                            font.pixelSize: 12
                        }

                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: {
                                if (page.installFinished) {
                                    page.installVisible = false
                                } else if (page.api && page.installRequestId !== "") {
                                    page.api.cancelInstall(page.installRequestId)
                                }
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

    /// 某个插件通过 `commands` 扩展点贡献的命令（只列已加载的插件）。
    function commandsOf(pluginId) {
        var out = []
        var all = api ? (api.commands || []) : []
        for (var i = 0; i < all.length; ++i) {
            if (all[i].pluginId === pluginId)
                out.push(all[i])
        }
        return out
    }

    Connections {
        target: page.api
        ignoreUnknownSignals: true

        function onToast(level, text) {
            page.showToast(level !== "error", text)
        }

        // 第一个 progress 到达时才知道 requestId，此时开对话框
        function onInstallProgress(requestId, stage, percent, detail) {
            if (!page.installVisible || page.installRequestId !== requestId)
                page.beginInstall(requestId)
            page.installStage   = stage
            page.installPercent = percent
            page.installDetail  = detail
        }

        function onInstallLog(requestId, line) {
            if (page.installRequestId !== requestId)
                return
            var lines = page.installLog.slice()
            lines.push(line)
            if (lines.length > 200)          // 只留最近 200 行，避免无限增长
                lines = lines.slice(lines.length - 200)
            page.installLog = lines
        }

        function onInstallFinished(requestId, ok, code, message) {
            if (page.installRequestId !== requestId)
                return
            page.installFinished = true
            page.installOk       = ok
            page.installMessage  = message !== "" ? message
                                                  : (ok ? "" : "失败：" + code)
            page.installPercent  = 100
        }
    }
    // ---- 登录限制遮罩（离线/未登录）----
    Rectangle {
        anchors.fill: parent
        z: 100
        visible: (typeof auth !== "undefined") && !auth.signedIn
        color: Qt.rgba(0, 0, 0, 0.45)

        ColumnLayout {
            anchors.centerIn: parent
            spacing: 10

            AppIcon {
                name: "lock"
                size: 28
                iconColor: Style.text
                Layout.alignment: Qt.AlignHCenter
            }
            Text {
                text: "该功能需要登录后使用"
                color: Style.text
                font.pixelSize: 15
                font.weight: Font.DemiBold
                font.family: Style.fontFamily
                Layout.alignment: Qt.AlignHCenter
            }
            Text {
                text: "当前为离线模式，登录后可解除限制"
                color: Style.text2
                font.pixelSize: 12
                font.family: Style.fontFamily
                Layout.alignment: Qt.AlignHCenter
            }
            AppButton {
                text: "登录"
                variant: "primary"
                Layout.alignment: Qt.AlignHCenter
                onClicked: if (typeof auth !== "undefined") auth.requestLogin()
            }
        }
    }

}
