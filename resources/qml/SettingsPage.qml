import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "components"

// 设置页（2026-11-19 UI 改版）
//
// 从「一根 800 行长滚动」改成「左侧二级导航 + 右侧分组卡片」。
// 所有配置项、热键捕获、规则编辑、IM 目录逻辑都保持原样，只重排视觉。
Rectangle {
    id: settingsPage
    color: "transparent"

    property bool capturing: false
    property string hotkeyStatus: ""
    property bool hotkeyStatusOk: false

    property var kindSelection: config.fileKindFilterKinds
    property string fileStatus: ""
    property int activeSection: 0

    readonly property var sections: [
        { "icon": "settings", "label": "通用" },
        { "icon": "filter",   "label": "文件索引" },
        { "icon": "inbox",    "label": "收件箱" },
        { "icon": "lock",     "label": "账号" },
        { "icon": "info",     "label": "关于" }
    ]

    function toggleKind(token, on) {
        var next = kindSelection.slice()
        var at = next.indexOf(token)
        if (on && at < 0)
            next.push(token)
        else if (!on && at >= 0)
            next.splice(at, 1)
        kindSelection = next
    }

    function jumpTo(index) {
        activeSection = index
        var targets = [generalSection, fileSection, inboxSection, accountSection, aboutSection]
        settingsFlick.contentY = Math.max(0, targets[index].y - 4)
    }

    RowLayout {
        anchors.fill: parent
        anchors.margins: Style.pagePadding
        spacing: Style.spacingL

        // ---- 左侧二级导航 ----
        Rectangle {
            Layout.preferredWidth: 148
            Layout.fillHeight: true
            radius: Style.radiusCard
            color: Style.surface
            border.width: 1
            border.color: Style.line

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 9
                spacing: 2

                Text {
                    Layout.leftMargin: 8
                    Layout.topMargin: 2
                    Layout.bottomMargin: 6
                    text: "设置"
                    color: Style.text
                    font.pixelSize: Style.fontTitle
                    font.weight: Font.DemiBold
                    font.family: Style.fontFamily
                }

                Repeater {
                    model: settingsPage.sections

                    delegate: Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 34
                        radius: Style.radiusControl
                        color: settingsPage.activeSection === index
                               ? Style.accentSoft
                               : sectionHover.hovered ? Style.surface2 : "transparent"

                        HoverHandler { id: sectionHover }

                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: 9
                            anchors.rightMargin: 8
                            spacing: 8

                            AppIcon {
                                name: modelData.icon
                                size: 14
                                iconColor: settingsPage.activeSection === index
                                           ? Style.accent : Style.text3
                            }
                            Text {
                                Layout.fillWidth: true
                                text: modelData.label
                                color: settingsPage.activeSection === index
                                       ? Style.accent : Style.text2
                                font.pixelSize: Style.fontBody
                                font.weight: settingsPage.activeSection === index
                                             ? Font.DemiBold : Font.Normal
                                font.family: Style.fontFamily
                                elide: Text.ElideRight
                            }
                        }

                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: settingsPage.jumpTo(index)
                        }
                    }
                }

                Item { Layout.fillHeight: true }

                Text {
                    Layout.leftMargin: 8
                    text: "v" + appVersion
                    color: Style.text3
                    font.pixelSize: 10
                    font.family: Style.fontFamily
                }
            }
        }

        // ---- 右侧内容 ----
        Flickable {
            id: settingsFlick
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            contentWidth: width
            contentHeight: settingsColumn.implicitHeight
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: AppScrollBar { }

            ColumnLayout {
                id: settingsColumn
                width: settingsFlick.width
                spacing: 14

                // ================= 通用 =================
                AppCard {
                    id: generalSection
                    Layout.fillWidth: true

                    ColumnLayout {
                        width: parent.width
                        spacing: 14

                        Text {
                            text: "通用"
                            color: Style.text
                            font.pixelSize: Style.fontSubtitle
                            font.weight: Font.DemiBold
                            font.family: Style.fontFamily
                        }

                        // 主题
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 8

                            Text {
                                text: "主题"
                                color: Style.text2
                                font.pixelSize: Style.fontBody
                                font.family: Style.fontFamily
                                Layout.preferredWidth: 76
                            }

                            AppButton {
                                text: "浅色"
                                icon: "sun"
                                small: true
                                variant: config.theme === "light" ? "primary" : "secondary"
                                onClicked: config.setTheme("light")
                            }
                            AppButton {
                                text: "深色"
                                icon: "moon"
                                small: true
                                variant: config.theme === "dark" ? "primary" : "secondary"
                                onClicked: config.setTheme("dark")
                            }
                            Item { Layout.fillWidth: true }
                        }

                        Rectangle { Layout.fillWidth: true; height: 1; color: Style.line }

                        // 日志级别
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 8

                            Text {
                                text: "日志级别"
                                color: Style.text2
                                font.pixelSize: Style.fontBody
                                font.family: Style.fontFamily
                                Layout.preferredWidth: 76
                            }

                            AppComboBox {
                                id: logLevelBox
                                Layout.preferredWidth: 140
                                model: ["trace", "debug", "info", "warn"]

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
                            Item { Layout.fillWidth: true }
                        }

                        Rectangle { Layout.fillWidth: true; height: 1; color: Style.line }

                        // 全局热键
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 8

                            Text {
                                text: "全局热键"
                                color: Style.text2
                                font.pixelSize: Style.fontBody
                                font.family: Style.fontFamily
                            }

                            RowLayout {
                                spacing: 8

                                Rectangle {
                                    width: 190
                                    height: 32
                                    radius: Style.radiusControl
                                    color: settingsPage.capturing ? Style.accentSoft : Style.surface
                                    border.width: settingsPage.capturing ? 1.5 : 1
                                    border.color: settingsPage.capturing ? Style.accent : Style.lineStrong

                                    Text {
                                        anchors.centerIn: parent
                                        text: settingsPage.capturing
                                              ? "请按下组合键…（Esc 取消）"
                                              : config.hotkeyModifiers + " + " + config.hotkeyKey
                                        color: settingsPage.capturing ? Style.accent : Style.text
                                        font.pixelSize: Style.fontBody
                                        font.weight: Font.DemiBold
                                        font.family: Style.fontFamily
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

                                AppButton {
                                    visible: settingsPage.capturing
                                    text: "取消"
                                    small: true
                                    variant: "ghost"
                                    onClicked: settingsPage.capturing = false
                                }
                            }

                            Text {
                                visible: settingsPage.hotkeyStatus.length > 0
                                text: settingsPage.hotkeyStatus
                                color: settingsPage.hotkeyStatusOk ? Style.success : Style.danger
                                font.pixelSize: 10
                                font.family: Style.fontFamily
                                wrapMode: Text.WordWrap
                                Layout.fillWidth: true
                            }

                            Text {
                                text: "至少一个修饰键（Ctrl / Alt / Shift / Win）+ 字母、数字、F1–F24 或空格"
                                color: Style.text3
                                font.pixelSize: 10
                                font.family: Style.fontFamily
                                wrapMode: Text.WordWrap
                                Layout.fillWidth: true
                            }
                        }

                        Rectangle { Layout.fillWidth: true; height: 1; color: Style.line }

                        // 开机启动
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 8

                            Text {
                                text: "开机启动"
                                color: Style.text2
                                font.pixelSize: Style.fontBody
                                font.family: Style.fontFamily
                                Layout.preferredWidth: 76
                            }

                            AppSwitch {
                                id: autoStartSwitch
                                checked: config.autoStart
                                onToggled: config.setAutoStart(checked)
                            }

                            Text {
                                Layout.fillWidth: true
                                text: config.autoStart ? "已随系统启动（HKCU\\...\\Run）" : "不随系统启动"
                                color: Style.text3
                                font.pixelSize: 10
                                font.family: Style.fontFamily
                                elide: Text.ElideRight
                            }
                        }
                    }
                }

                // ================= 文件索引 =================
                AppCard {
                    id: fileSection
                    Layout.fillWidth: true

                    ColumnLayout {
                        width: parent.width
                        spacing: 12

                        Text {
                            text: "文件索引"
                            color: Style.text
                            font.pixelSize: Style.fontSubtitle
                            font.weight: Font.DemiBold
                            font.family: Style.fontFamily
                        }

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 8

                            Text {
                                text: "种类过滤"
                                color: Style.text2
                                font.pixelSize: Style.fontBody
                                font.family: Style.fontFamily
                                Layout.preferredWidth: 76
                            }

                            AppComboBox {
                                id: kindModeBox
                                Layout.preferredWidth: 200
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
                            Item { Layout.fillWidth: true }
                        }

                        Flow {
                            Layout.fillWidth: true
                            spacing: 4
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

                                delegate: RowLayout {
                                    width: 92
                                    height: 26
                                    spacing: 6

                                    AppCheckbox {
                                        checked: settingsPage.kindSelection.indexOf(modelData.token) >= 0
                                        onToggled: settingsPage.toggleKind(modelData.token, checked)
                                    }
                                    Text {
                                        text: modelData.label
                                        color: Style.text2
                                        font.pixelSize: 11
                                        font.family: Style.fontFamily
                                    }
                                }
                            }
                        }

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 8

                            AppButton {
                                text: "应用白名单"
                                variant: "primary"
                                small: true
                                onClicked: {
                                    var mode = kindModeBox.model[kindModeBox.currentIndex].key
                                    config.setFileKindFilter(mode, settingsPage.kindSelection)
                                    settingsPage.fileStatus = mode === "all"
                                        ? "已恢复：显示全部文件（立即生效）"
                                        : "已应用：立即生效，无需重新扫描索引"
                                }
                            }
                            AppButton {
                                text: "重建索引"
                                variant: "secondary"
                                small: true
                                onClicked: {
                                    backend.rebuildFileIndex()
                                    settingsPage.fileStatus = "已开始重建（全量扫描；期间仍可用旧索引搜索）"
                                }
                            }
                            Text {
                                Layout.fillWidth: true
                                text: settingsPage.fileStatus
                                color: Style.success
                                font.pixelSize: 10
                                font.family: Style.fontFamily
                                elide: Text.ElideRight
                            }
                        }

                        Text {
                            text: "白名单只影响搜索结果，不会删除索引；切换无需重扫。"
                            color: Style.text3
                            font.pixelSize: 10
                            font.family: Style.fontFamily
                        }

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 8

                            Text {
                                text: "工程目录"
                                color: Style.text2
                                font.pixelSize: Style.fontBody
                                font.family: Style.fontFamily
                                Layout.preferredWidth: 76
                            }

                            AppComboBox {
                                id: projectPolicyBox
                                Layout.preferredWidth: 200
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
                            Item { Layout.fillWidth: true }
                        }

                        Text {
                            text: "工程目录 = 含 .git / CMakeLists.txt / package.json 等哨兵的目录。改这项会重新扫描索引。"
                            color: Style.text3
                            font.pixelSize: 10
                            font.family: Style.fontFamily
                            wrapMode: Text.WordWrap
                            Layout.fillWidth: true
                        }
                    }
                }

                // ================= 收件箱 =================
                AppCard {
                    id: inboxSection
                    Layout.fillWidth: true

                    ColumnLayout {
                        width: parent.width
                        spacing: 12

                        Text {
                            text: "收件箱"
                            color: Style.text
                            font.pixelSize: Style.fontSubtitle
                            font.weight: Font.DemiBold
                            font.family: Style.fontFamily
                        }

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 8

                            Text {
                                text: "下载接管"
                                color: Style.text2
                                font.pixelSize: Style.fontBody
                                font.family: Style.fontFamily
                                Layout.preferredWidth: 76
                            }

                            AppSwitch {
                                id: inboxSwitch
                                checked: config.inboxEnabled
                                onToggled: config.setInboxEnabled(checked)
                            }

                            Text {
                                Layout.fillWidth: true
                                text: config.inboxEnabled
                                      ? "已开启：只接管开启之后完成的新下载"
                                      : "未开启（不会移动任何文件）"
                                color: Style.text3
                                font.pixelSize: 10
                                font.family: Style.fontFamily
                                elide: Text.ElideRight
                            }
                        }

                        Text {
                            text: "收件箱：" + config.inboxPath
                            color: Style.text3
                            font.pixelSize: 10
                            font.family: Style.fontFamily
                            elide: Text.ElideMiddle
                            Layout.fillWidth: true
                        }

                        Text {
                            text: "下载完成判定通过后移入收件箱（可在「最近下载」页打开/定位/撤销）；绝不删除源文件，失败时保持原位。"
                            color: Style.text3
                            font.pixelSize: 10
                            font.family: Style.fontFamily
                            wrapMode: Text.WordWrap
                            Layout.fillWidth: true
                        }

                        Rectangle { Layout.fillWidth: true; height: 1; color: Style.line }

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 8

                            Text {
                                text: "自动归档"
                                color: Style.text2
                                font.pixelSize: Style.fontBody
                                font.family: Style.fontFamily
                                Layout.preferredWidth: 76
                            }

                            AppSwitch {
                                checked: config.inboxAutoOrganize
                                onToggled: config.setInboxAutoOrganize(checked)
                            }

                            Text {
                                Layout.fillWidth: true
                                text: config.inboxAutoOrganize
                                      ? "自动执行 move 规则（默认关闭）"
                                      : "只给建议，不移动文件"
                                color: Style.text3
                                font.pixelSize: 10
                                font.family: Style.fontFamily
                                elide: Text.ElideRight
                            }
                        }

                        Text {
                            text: "按 priority 从小到大首个命中；action = suggest / move / skip / notify。未开启自动归档时，move 也只显示在「最近下载」的建议里。"
                            color: Style.text3
                            font.pixelSize: 10
                            font.family: Style.fontFamily
                            wrapMode: Text.WordWrap
                            Layout.fillWidth: true
                        }

                        // ---- 规则编辑器 ----
                        ColumnLayout {
                            id: rulesSection
                            Layout.fillWidth: true
                            spacing: 6

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

                            Text {
                                text: "归档规则"
                                color: Style.text3
                                font.pixelSize: 10
                                font.weight: Font.DemiBold
                                font.letterSpacing: 1.0
                                font.family: Style.fontFamily
                            }

                            Repeater {
                                model: ruleModel

                                delegate: ColumnLayout {
                                    id: ruleDelegate
                                    property int rowIndex: index
                                    Layout.fillWidth: true
                                    spacing: 4

                                    RowLayout {
                                        Layout.fillWidth: true
                                        spacing: 4

                                        AppTextField {
                                            text: model.name
                                            placeholder: "名称"
                                            compact: true
                                            Layout.preferredWidth: 110
                                            onUserEdited: ruleModel.setProperty(index, "name", text)
                                        }
                                        AppTextField {
                                            text: model.kind
                                            placeholder: "种类 doc,pdf"
                                            compact: true
                                            Layout.preferredWidth: 100
                                            onUserEdited: ruleModel.setProperty(index, "kind", text)
                                        }
                                        AppTextField {
                                            text: model.size
                                            placeholder: "大小 >10mb"
                                            compact: true
                                            Layout.preferredWidth: 90
                                            onUserEdited: ruleModel.setProperty(index, "size", text)
                                        }
                                        AppIconButton {
                                            icon: "trash"
                                            iconSize: 13
                                            danger: true
                                            tooltip: "删除规则"
                                            onClicked: ruleModel.remove(index)
                                        }
                                    }

                                    RowLayout {
                                        Layout.fillWidth: true
                                        spacing: 4

                                        AppComboBox {
                                            Layout.preferredWidth: 96
                                            model: [ "suggest", "move", "skip", "notify" ]
                                            Component.onCompleted: {
                                                var actions = [ "suggest", "move", "skip", "notify" ]
                                                var at = actions.indexOf(action)
                                                currentIndex = at >= 0 ? at : 0
                                            }
                                            onActivated: ruleModel.setProperty(ruleDelegate.rowIndex,
                                                                                "action", currentText)
                                        }
                                        AppTextField {
                                            text: model.dest
                                            placeholder: "去向 {inbox}/Docs/{yyyy}"
                                            compact: true
                                            Layout.fillWidth: true
                                            onUserEdited: ruleModel.setProperty(index, "dest", text)
                                        }
                                    }
                                }
                            }

                            RowLayout {
                                spacing: 8

                                AppButton {
                                    text: "添加规则"
                                    variant: "secondary"
                                    small: true
                                    onClicked: ruleModel.append({ name: "", kind: "", size: "",
                                                                  action: "suggest", dest: "" })
                                }
                                AppButton {
                                    text: "预览"
                                    variant: "ghost"
                                    small: true
                                    onClicked: rulesSection.preview()
                                }
                                AppButton {
                                    text: "保存规则"
                                    variant: "primary"
                                    small: true
                                    onClicked: rulesSection.saveRules()
                                }
                                Text {
                                    id: rulesStatus
                                    color: Style.success
                                    font.pixelSize: 10
                                    font.family: Style.fontFamily
                                }
                            }

                            Text {
                                id: previewLabel
                                visible: text.length > 0
                                color: Style.text3
                                font.pixelSize: 10
                                font.family: Style.fontFamily
                                wrapMode: Text.WordWrap
                                Layout.fillWidth: true
                            }
                        }

                        Rectangle { Layout.fillWidth: true; height: 1; color: Style.line }

                        // ---- IM 目录 ----
                        ColumnLayout {
                            id: imSection
                            Layout.fillWidth: true
                            spacing: 6

                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 8

                                Text {
                                    text: "IM 接收目录"
                                    color: Style.text2
                                    font.pixelSize: Style.fontBody
                                    font.family: Style.fontFamily
                                    Layout.preferredWidth: 76
                                }

                                AppSwitch {
                                    checked: config.imEnabled
                                    onToggled: config.setImEnabled(checked)
                                }

                                Text {
                                    Layout.fillWidth: true
                                    text: config.imEnabled
                                          ? "已开启：复制到这些目录的完成文件会进收件箱"
                                          : "未开启（默认，不碰 IM 目录）"
                                    color: Style.text3
                                    font.pixelSize: 10
                                    font.family: Style.fontFamily
                                    elide: Text.ElideRight
                                }
                            }

                            Text {
                                text: "多个目录用分号分隔；修改目录后需重启应用（watcher 只在启动时建）。"
                                color: Style.text3
                                font.pixelSize: 10
                                font.family: Style.fontFamily
                                wrapMode: Text.WordWrap
                                Layout.fillWidth: true
                            }

                            AppTextField {
                                id: imDirsField
                                Layout.fillWidth: true
                                text: config.imDirs.join(";")
                                placeholder: "C:/Users/me/Documents/WeChat Files;..."
                            }

                            RowLayout {
                                spacing: 6

                                AppButton {
                                    text: "扫描常见目录"
                                    variant: "secondary"
                                    small: true
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
                                AppButton {
                                    text: "保存目录"
                                    variant: "primary"
                                    small: true
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
                                Text {
                                    id: imStatus
                                    color: Style.success
                                    font.pixelSize: 10
                                    font.family: Style.fontFamily
                                }
                            }
                        }
                    }
                }

                // ================= 账号（独立栏） =================
                AppCard {
                    id: accountSection
                    Layout.fillWidth: true

                    Component.onCompleted: {
                        if (typeof auth !== "undefined" && auth.userName !== "")
                            accountUserField.text = auth.userName
                    }

                    ColumnLayout {
                        width: parent.width
                        spacing: 8

                        Text {
                            text: "账号与密码"
                            color: Style.text
                            font.pixelSize: Style.fontSubtitle
                            font.weight: Font.DemiBold
                            font.family: Style.fontFamily
                        }

                        Text {
                            Layout.fillWidth: true
                            text: (typeof auth !== "undefined") ? auth.statusText : "账号模块未就绪"
                            color: (typeof auth !== "undefined" && auth.signedIn)
                                   ? Style.success : Style.text2
                            font.pixelSize: 11
                            font.family: Style.fontFamily
                            wrapMode: Text.WordWrap
                        }

                        AppTextField {
                            id: accountUserField
                            Layout.fillWidth: true
                            placeholder: "账号"
                        }
                        AppTextField {
                            id: accountPassField
                            Layout.fillWidth: true
                            placeholder: "密码（至少 6 位）"
                            echoMode: TextInput.Password
                            onAccepted: if (typeof auth !== "undefined")
                                            auth.saveAccount(accountUserField.text, accountPassField.text)
                        }

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 8

                            AppButton {
                                text: "保存账号"
                                variant: "primary"
                                onClicked: if (typeof auth !== "undefined")
                                               auth.saveAccount(accountUserField.text, accountPassField.text)
                            }
                            AppButton {
                                text: "登录"
                                variant: "ghost"
                                visible: typeof auth !== "undefined" && auth.hasAccount && !auth.signedIn
                                onClicked: if (typeof auth !== "undefined")
                                               auth.login(accountUserField.text, accountPassField.text)
                            }
                            AppButton {
                                text: "退出登录"
                                variant: "ghost"
                                visible: typeof auth !== "undefined" && auth.signedIn
                                onClicked: if (typeof auth !== "undefined") auth.logout()
                            }
                            Item { Layout.fillWidth: true }
                        }

                        Text {
                            Layout.fillWidth: true
                            text: "账号只保存在本机安全模块数据目录（security/account.json），密码用 PBKDF2 存哈希；不联网也能登录。离线登录时设备同步、插件市场等功能受限。"
                            color: Style.text3
                            font.pixelSize: 10
                            font.family: Style.fontFamily
                            wrapMode: Text.WordWrap
                        }
                    }
                }

                // ================= 关于 =================
                AppCard {
                    id: aboutSection
                    Layout.fillWidth: true

                    ColumnLayout {
                        width: parent.width
                        spacing: 6

                        Text {
                            text: "关于"
                            color: Style.text
                            font.pixelSize: Style.fontSubtitle
                            font.weight: Font.DemiBold
                            font.family: Style.fontFamily
                        }
                        Text {
                            text: "Assistant v" + appVersion + " · Windows 桌面助手"
                            color: Style.text2
                            font.pixelSize: Style.fontBody
                            font.family: Style.fontFamily
                        }
                        Text {
                            text: "配置与数据库位于 %LOCALAPPDATA%/AssistantTeam/Assistant/；主题、日志级别、文件索引与收件箱设置即时生效。"
                            color: Style.text3
                            font.pixelSize: 10
                            font.family: Style.fontFamily
                            wrapMode: Text.WordWrap
                            Layout.fillWidth: true
                        }
                    }
                }
            }
        }
    }

    // ---- 隐藏的按键捕获器（放在布局之外，避免影响 ColumnLayout）----
    Item {
        id: keyCapture
        width: 0
        height: 0
        focus: settingsPage.capturing

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

            if (event.key === Qt.Key_Escape && mods.length === 0) {
                event.accepted = true
                settingsPage.capturing = false
                return
            }

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
