import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// 设置页（Phase 3 Step 6 + 收尾）
// 主题 / 日志级别 / 全局热键（可自定义）/ 开机启动
// 主题色统一走 Style 单例（同目录 qmldir 声明的 singleton）。
Rectangle {
    id: settingsPage
    color: "transparent"

    readonly property bool isDark:      Style.isDark
    readonly property color textColor:  Style.textColor
    readonly property color mutedColor: Style.mutedColor
    readonly property color accentColor: Style.accentColor
    readonly property color borderColor: Style.borderColor
    readonly property color panelColor: Style.panelColor

    // ---- 热键捕获状态 ----
    property bool capturing: false
    property string hotkeyStatus: ""
    property bool   hotkeyStatusOk: false

    // ---- 文件索引范围（Phase B）----
    // kindSelection 用本地副本：勾选过程中不写配置，点「应用」才落盘。
    // 初值直接绑定配置；用户勾选后改成普通数组（绑定断开），避免每次点击都写盘。
    property var kindSelection: config.fileKindFilterKinds
    property string fileStatus: ""

    function toggleKind(token, on) {
        var next = kindSelection.slice()
        var at = next.indexOf(token)
        if (on && at < 0)
            next.push(token)
        else if (!on && at >= 0)
            next.splice(at, 1)
        kindSelection = next
    }

    // 内容比窗口高（Phase B 加了文件索引范围），用 Flickable 滚动，
    // 否则最后一项会被裁掉且没有任何提示。
    Flickable {
        id: settingsFlick
        anchors.fill: parent
        anchors.margins: 8
        clip: true
        contentWidth: width
        contentHeight: settingsColumn.implicitHeight
        ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

    ColumnLayout {
        id: settingsColumn
        width: settingsFlick.width
        spacing: 20

        Text {
            text: "设置"
            color: textColor
            font.pixelSize: Style.titleSize
            font.bold: true
        }

        // ---- 主题 ----
        ColumnLayout {
            spacing: 8

            Text {
                text: "主题"
                color: mutedColor
                font.pixelSize: 12
            }

            RowLayout {
                spacing: 8

                Rectangle {
                    width: 70
                    height: 32
                    radius: Style.radiusSmall
                    color: config.theme === "light" ? accentColor : panelColor
                    border.color: borderColor

                    Text {
                        anchors.centerIn: parent
                        text: "浅色"
                        color: config.theme === "light" ? Style.accentTextColor : textColor
                        font.pixelSize: Style.bodySize
                    }

                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: config.setTheme("light")
                    }
                }

                Rectangle {
                    width: 70
                    height: 32
                    radius: Style.radiusSmall
                    color: config.theme === "dark" ? accentColor : panelColor
                    border.color: borderColor

                    Text {
                        anchors.centerIn: parent
                        text: "深色"
                        color: config.theme === "dark" ? Style.accentTextColor : textColor
                        font.pixelSize: Style.bodySize
                    }

                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: config.setTheme("dark")
                    }
                }
            }
        }

        // ---- 日志级别 ----
        ColumnLayout {
            spacing: 8

            Text {
                text: "日志级别"
                color: mutedColor
                font.pixelSize: 12
            }

            ComboBox {
                id: logLevelBox
                model: ["trace", "debug", "info", "warn"]
                Layout.preferredWidth: 140

                Component.onCompleted: {
                    for (let i = 0; i < model.length; i++) {
                        if (model[i] === config.logLevel) {
                            currentIndex = i
                            break
                        }
                    }
                }

                onActivated: config.setLogLevel(currentText)
            }
        }

        // ---- 文件索引范围（Phase B：种类过滤 + 工程排除）----
        ColumnLayout {
            Layout.fillWidth: true
            spacing: 8

            Text {
                text: "文件索引范围"
                color: mutedColor
                font.pixelSize: 12
            }

            RowLayout {
                spacing: 8

                Text {
                    text: "种类过滤"
                    color: textColor
                    font.pixelSize: 12
                    Layout.preferredWidth: 64
                }

                ComboBox {
                    id: kindModeBox
                    Layout.preferredWidth: 190
                    model: [
                        { "key": "all",       "label": "全部文件（不过滤）" },
                        { "key": "whitelist", "label": "只保留选中的种类" },
                        { "key": "blacklist", "label": "排除选中的种类" }
                    ]
                    textRole: "label"

                    Component.onCompleted: {
                        for (var i = 0; i < model.length; ++i) {
                            if (model[i].key === config.fileKindFilterMode) {
                                currentIndex = i
                                break
                            }
                        }
                    }
                }
            }

            Grid {
                columns: 4
                columnSpacing: 4
                rowSpacing: 0
                visible: kindModeBox.currentIndex > 0

                Repeater {
                    model: [
                        { "token": "doc",   "label": "文档" },
                        { "token": "sheet", "label": "表格" },
                        { "token": "slide", "label": "演示" },
                        { "token": "pdf",   "label": "PDF" },
                        { "token": "img",   "label": "图片" },
                        { "token": "vid",   "label": "视频" },
                        { "token": "aud",   "label": "音频" },
                        { "token": "zip",   "label": "压缩包" },
                        { "token": "exe",   "label": "安装包" },
                        { "token": "code",  "label": "代码" },
                        { "token": "font",  "label": "字体" },
                        { "token": "data",  "label": "数据" },
                        { "token": "ebook", "label": "电子书" },
                        { "token": "lnk",   "label": "快捷方式" },
                        { "token": "disk",  "label": "磁盘镜像" },
                        { "token": "other", "label": "其他" }
                    ]

                    delegate: CheckBox {
                        text: modelData.label
                        font.pixelSize: 12

                        Component.onCompleted:
                            checked = settingsPage.kindSelection.indexOf(modelData.token) >= 0

                        onToggled: settingsPage.toggleKind(modelData.token, checked)
                    }
                }
            }

            RowLayout {
                spacing: 10

                Button {
                    text: "应用白名单"

                    onClicked: {
                        var mode = kindModeBox.model[kindModeBox.currentIndex].key
                        config.setFileKindFilter(mode, settingsPage.kindSelection)
                        settingsPage.fileStatus = mode === "all"
                            ? "已恢复：显示全部文件（立即生效）"
                            : "已应用：立即生效，无需重新扫描索引"
                    }
                }

                Text {
                    text: settingsPage.fileStatus
                    color: mutedColor
                    font.pixelSize: Style.captionSize
                    Layout.fillWidth: true
                    elide: Text.ElideRight
                }
            }

            Text {
                text: "白名单只影响搜索结果，不会删除索引；切换无需重扫。"
                color: mutedColor
                font.pixelSize: Style.captionSize
            }

            RowLayout {
                spacing: 8
                Layout.topMargin: 4

                Text {
                    text: "工程目录"
                    color: textColor
                    font.pixelSize: 12
                    Layout.preferredWidth: 64
                }

                ComboBox {
                    id: projectPolicyBox
                    Layout.preferredWidth: 190
                    model: [
                        { "key": "skip",   "label": "跳过（默认，推荐）" },
                        { "key": "demote", "label": "入库但降权 30%" },
                        { "key": "index",  "label": "如实入库" }
                    ]
                    textRole: "label"

                    Component.onCompleted: {
                        for (var i = 0; i < model.length; ++i) {
                            if (model[i].key === config.fileProjectPolicy) {
                                currentIndex = i
                                break
                            }
                        }
                    }

                    onActivated: {
                        config.setFileProjectPolicy(model[currentIndex].key)
                        settingsPage.fileStatus = "已保存；正在后台重新建立索引…"
                    }
                }
            }

            Text {
                text: "工程目录 = 含 .git / CMakeLists.txt / package.json 等哨兵的目录。改这项会重新扫描索引。"
                color: mutedColor
                font.pixelSize: Style.captionSize
                wrapMode: Text.WordWrap
                Layout.maximumWidth: 460
            }
        }

        // ---- 下载接管（Phase C）----
        ColumnLayout {
            Layout.fillWidth: true
            spacing: 8

            Text {
                text: "下载接管（收件箱）"
                color: mutedColor
                font.pixelSize: 12
            }

            RowLayout {
                spacing: 8

                Switch {
                    id: inboxSwitch
                    checked: config.inboxEnabled
                    onToggled: config.setInboxEnabled(checked)
                }

                Text {
                    text: config.inboxEnabled
                          ? "已开启：只接管开启之后完成的新下载"
                          : "未开启（不会移动任何文件）"
                    color: mutedColor
                    font.pixelSize: 12
                }
            }

            Text {
                text: "收件箱：" + config.inboxPath
                color: mutedColor
                font.pixelSize: Style.captionSize
                elide: Text.ElideMiddle
                Layout.maximumWidth: 460
            }

            Text {
                text: "下载完成判定通过后移入收件箱（可在「最近下载」页打开/定位/撤销）；"
                      + "绝不删除源文件，失败时保持原位。"
                color: mutedColor
                font.pixelSize: Style.captionSize
                wrapMode: Text.WordWrap
                Layout.maximumWidth: 460
            }
        }

        // ---- 下载归档规则（S11：可编辑 + dry-run）----
        ColumnLayout {
            id: rulesSection
            Layout.fillWidth: true
            spacing: 8

            Text {
                text: "下载归档规则"
                color: mutedColor
                font.pixelSize: 12
            }

            RowLayout {
                spacing: 8

                Switch {
                    checked: config.inboxAutoOrganize
                    onToggled: config.setInboxAutoOrganize(checked)
                }

                Text {
                    text: config.inboxAutoOrganize
                          ? "自动执行 move 规则（默认关闭）"
                          : "只给建议，不移动文件"
                    color: mutedColor
                    font.pixelSize: 12
                }
            }

            Text {
                text: "按 priority 从小到大首个命中；action = suggest / move / skip / notify。"
                      + " 未开启自动归档时，move 也只显示在「最近下载」的建议里。"
                color: mutedColor
                font.pixelSize: Style.captionSize
                wrapMode: Text.WordWrap
                Layout.maximumWidth: 460
            }

            ListModel { id: ruleModel }

            Component.onCompleted: rulesSection.loadRules()

            function loadRules() {
                ruleModel.clear()
                var rules = config.inboxRules
                for (var i = 0; i < rules.length; ++i) {
                    var r = rules[i]
                    ruleModel.append({
                        name: r.name || "",
                        kind: r.kind || "",
                        size: r.size || "",
                        action: r.action || "suggest",
                        dest: r.dest || ""
                    })
                }
            }

            function ruleArray() {
                var arr = []
                for (var i = 0; i < ruleModel.count; ++i) {
                    var r = ruleModel.get(i)
                    arr.push({
                        name: r.name,
                        kind: r.kind,
                        size: r.size,
                        action: r.action,
                        dest: r.dest,
                        priority: 100,
                        enabled: true
                    })
                }
                return arr
            }

            function preview() {
                var result = inbox.previewRules(ruleArray())
                if (result.length === 0) {
                    previewLabel.text = "当前收件箱没有待整理文件。"
                    return
                }
                var lines = []
                for (var i = 0; i < result.length; ++i) {
                    var item = result[i]
                    if (item.matched)
                        lines.push(item.name + " → " + item.rule + " / "
                                   + item.action + " / " + item.dest)
                    else
                        lines.push(item.name + " → 未命中")
                }
                previewLabel.text = lines.join("\n")
            }

            function saveRules() {
                config.setInboxRules(ruleArray())
                rulesStatus.text = "已保存（" + ruleModel.count + " 条）"
            }

            Repeater {
                model: ruleModel

                delegate: ColumnLayout {
                    id: ruleDelegate
                    property int rowIndex: index
                    Layout.fillWidth: true
                    spacing: 2

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 4

                        TextField {
                            text: model.name
                            placeholderText: "名称"
                            Layout.preferredWidth: 120
                            onTextEdited: ruleModel.setProperty(index, "name", text)
                        }
                        TextField {
                            text: model.kind
                            placeholderText: "种类 doc,pdf"
                            Layout.preferredWidth: 110
                            onTextEdited: ruleModel.setProperty(index, "kind", text)
                        }
                        TextField {
                            text: model.size
                            placeholderText: "大小 >10mb"
                            Layout.preferredWidth: 95
                            onTextEdited: ruleModel.setProperty(index, "size", text)
                        }
                        Button {
                            text: "×"
                            onClicked: ruleModel.remove(index)
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 4

                        ComboBox {
                            model: [ "suggest", "move", "skip", "notify" ]
                            Component.onCompleted: {
                                var actions = [ "suggest", "move", "skip", "notify" ]
                                var at = actions.indexOf(action)
                                currentIndex = at >= 0 ? at : 0
                            }
                            onActivated: ruleModel.setProperty(ruleDelegate.rowIndex,
                                                               "action", currentText)
                        }
                        TextField {
                            text: model.dest
                            placeholderText: "去向 {inbox}/Docs/{yyyy}"
                            Layout.fillWidth: true
                            onTextEdited: ruleModel.setProperty(index, "dest", text)
                        }
                    }
                }
            }

            RowLayout {
                spacing: 8

                Button {
                    text: "添加规则"
                    onClicked: ruleModel.append({ name: "", kind: "", size: "",
                                                  action: "suggest", dest: "" })
                }
                Button {
                    text: "预览（dry-run）"
                    onClicked: rulesSection.preview()
                }
                Button {
                    text: "保存规则"
                    onClicked: rulesSection.saveRules()
                }
                Text {
                    id: rulesStatus
                    color: accentColor
                    font.pixelSize: Style.captionSize
                }
            }

            Text {
                id: previewLabel
                visible: text.length > 0
                color: mutedColor
                font.pixelSize: Style.captionSize
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
                Layout.maximumWidth: 460
            }
        }

        // ---- IM 接收目录（S11，默认关闭，需用户确认）----
        ColumnLayout {
            id: imSection
            Layout.fillWidth: true
            spacing: 8

            Text {
                text: "IM 接收目录（QQ / 微信 / 钉钉）"
                color: mutedColor
                font.pixelSize: 12
            }

            RowLayout {
                spacing: 8

                Switch {
                    checked: config.imEnabled
                    onToggled: config.setImEnabled(checked)
                }

                Text {
                    text: config.imEnabled
                          ? "已开启：复制到这些目录的完成文件会进收件箱"
                          : "未开启（默认，不碰 IM 目录）"
                    color: mutedColor
                    font.pixelSize: 12
                }
            }

            Text {
                text: "多个目录用分号分隔；修改目录后需重启应用（watcher 只在启动时建）。"
                color: mutedColor
                font.pixelSize: Style.captionSize
                wrapMode: Text.WordWrap
                Layout.maximumWidth: 460
            }

            TextField {
                id: imDirsField
                text: config.imDirs.join(";")
                placeholderText: "C:/Users/me/Documents/WeChat Files;..."
                Layout.fillWidth: true
                Layout.maximumWidth: 460
            }

            RowLayout {
                spacing: 4

                Button {
                    text: "扫描常见目录"
                    onClicked: {
                        var found = inbox.imCandidates()
                        if (found.length === 0) {
                            imStatus.text = "没有发现常见 IM 目录"
                        } else {
                            imDirsField.text = found.join(";")
                            imStatus.text = "已填入 " + found.length + " 个目录，请确认后保存"
                        }
                    }
                }
                Button {
                    text: "保存目录"
                    onClicked: {
                        var parts = imDirsField.text.split(";")
                        var dirs = []
                        for (var i = 0; i < parts.length; ++i) {
                            var d = parts[i].trim()
                            if (d.length > 0)
                                dirs.push(d)
                        }
                        config.setImDirs(dirs)
                        imStatus.text = "已保存（" + dirs.length + " 个目录），重启后监听"
                    }
                }
            }

            Text {
                id: imStatus
                color: accentColor
                font.pixelSize: Style.captionSize
            }
        }

        // ---- 全局热键（可自定义，Phase 3 收尾）----
        ColumnLayout {
            spacing: 8

            Text {
                text: "全局热键"
                color: mutedColor
                font.pixelSize: 12
            }

            RowLayout {
                spacing: 8

                // 点击进入捕获态，接着按下的组合键即为新热键
                Rectangle {
                    width: 190
                    height: 32
                    radius: Style.radiusSmall
                    color: settingsPage.capturing ? accentColor : panelColor
                    border.color: settingsPage.capturing ? accentColor : borderColor
                    border.width: settingsPage.capturing ? 2 : 1

                    Text {
                        anchors.centerIn: parent
                        text: settingsPage.capturing
                              ? "请按下组合键…（Esc 取消）"
                              : config.hotkeyModifiers + " + " + config.hotkeyKey
                        color: settingsPage.capturing ? Style.accentTextColor : textColor
                        font.pixelSize: Style.bodySize
                        font.bold: !settingsPage.capturing
                    }

                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            settingsPage.capturing = true
                            settingsPage.hotkeyStatus = ""
                            keyCapture.forceActiveFocus()
                        }
                    }
                }

                Rectangle {
                    visible: settingsPage.capturing
                    width: 56
                    height: 32
                    radius: Style.radiusSmall
                    color: borderColor

                    Text {
                        anchors.centerIn: parent
                        text: "取消"
                        color: textColor
                        font.pixelSize: 12
                    }

                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: settingsPage.capturing = false
                    }
                }
            }

            Text {
                visible: settingsPage.hotkeyStatus.length > 0
                text: settingsPage.hotkeyStatus
                color: settingsPage.hotkeyStatusOk ? mutedColor : Style.dangerColor
                font.pixelSize: Style.captionSize
                wrapMode: Text.WordWrap
                Layout.maximumWidth: 460
            }

            Text {
                text: "至少包含一个修饰键（Ctrl / Alt / Shift / Win）+ 字母、数字、F1–F24 或空格"
                color: mutedColor
                font.pixelSize: Style.captionSize
            }
        }

        // ---- 开机启动（Phase 3 收尾）----
        ColumnLayout {
            spacing: 8

            Text {
                text: "开机启动"
                color: mutedColor
                font.pixelSize: 12
            }

            RowLayout {
                spacing: 8

                Switch {
                    id: autoStartSwitch
                    checked: config.autoStart
                    onToggled: config.setAutoStart(checked)
                }

                Text {
                    text: config.autoStart ? "已随系统启动（写入用户注册表 Run 键）"
                                           : "不随系统启动"
                    color: mutedColor
                    font.pixelSize: 12
                }
            }
        }

        // ---- 关于 ----
        Text {
            text: "Assistant v" + appVersion + " · Phase 3"
            color: mutedColor
            font.pixelSize: Style.captionSize
            Layout.fillWidth: true
        }
    }
    }

    // ---- 隐藏的按键捕获器（放在布局之外，避免影响 ColumnLayout）----
    Item {
        id: keyCapture
        width: 0
        height: 0
        focus: settingsPage.capturing

        // 把 Qt 的按键值翻译成 HotkeyManager::parseKey 认识的名字
        function keyNameOf(k) {
            if (k >= Qt.Key_A && k <= Qt.Key_Z) return String.fromCharCode(k)
            if (k >= Qt.Key_0 && k <= Qt.Key_9) return String.fromCharCode(k)
            if (k >= Qt.Key_F1 && k <= Qt.Key_F24) return "F" + (k - Qt.Key_F1 + 1)
            if (k === Qt.Key_Space)  return "Space"
            if (k === Qt.Key_Escape) return "Escape"
            if (k === Qt.Key_Tab)    return "Tab"
            if (k === Qt.Key_Return || k === Qt.Key_Enter) return "Return"
            return ""
        }

        Keys.onPressed: function(event) {
            if (!settingsPage.capturing) return

            const mods = []
            if (event.modifiers & Qt.ControlModifier) mods.push("Ctrl")
            if (event.modifiers & Qt.AltModifier)     mods.push("Alt")
            if (event.modifiers & Qt.ShiftModifier)   mods.push("Shift")
            if (event.modifiers & Qt.MetaModifier)    mods.push("Win")

            // 单独按 Esc：取消捕获
            if (event.key === Qt.Key_Escape && mods.length === 0) {
                event.accepted = true
                settingsPage.capturing = false
                return
            }

            // 只按下修饰键本身：等待主键，不当作组合键
            if (event.key === Qt.Key_Control || event.key === Qt.Key_Alt ||
                event.key === Qt.Key_Shift   || event.key === Qt.Key_Meta) {
                event.accepted = true
                return
            }

            event.accepted = true

            if (mods.length === 0) {
                settingsPage.hotkeyStatus = "请至少包含一个修饰键（Ctrl / Alt / Shift / Win）"
                settingsPage.hotkeyStatusOk = false
                return
            }

            const keyName = keyNameOf(event.key)
            if (keyName === "") {
                settingsPage.hotkeyStatus = "不支持的按键：请用字母 / 数字 / F1–F24 / 空格 / Tab / 回车"
                settingsPage.hotkeyStatusOk = false
                return
            }

            const combo = mods.join("+")
            if (panel.applyHotkey(combo, keyName)) {
                settingsPage.hotkeyStatus = "已更新为 " + mods.join(" + ") + " + " + keyName
                settingsPage.hotkeyStatusOk = true
                settingsPage.capturing = false
            } else {
                settingsPage.hotkeyStatus = "该组合键被其他程序占用，注册失败（已保留原热键 " +
                                            config.hotkeyModifiers + " + " + config.hotkeyKey + "）"
                settingsPage.hotkeyStatusOk = false
            }
        }
    }
}
