import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts

// 设备同步页（Phase: 联网功能）
// 三张卡片：连接 / 同步范围 / 同步，下面折叠发送文件，再接冲突记录。
// 主题色统一走 Style 单例。C++ 侧由 SyncBridge 注入为 `sync`。
Rectangle {
    id: syncPage
    color: "transparent"

    readonly property color textColor:  Style.textColor
    readonly property color mutedColor: Style.mutedColor
    readonly property color accentColor: Style.accentColor
    readonly property color borderColor: Style.borderColor
    readonly property color panelColor: Style.panelColor

    // 「检查共享」的结果常驻：保存、同步后都不清空，只有重新检查才更新
    property var sharedCheckResult: null
    // 本机共享名与地址（对方填 \\本机IP\共享名）
    property string localShareName: "SyncHub"
    property string localIp: ""

    // 角色由共享目录的形态决定：
    //   本地路径（E:\SyncHub）    → 主机：我把文件夹共享出去
    //   \\IP\共享名 / //IP/共享名 → 客户端：我连别人的共享
    readonly property string sharedDirInput: sharedDirField.text.trim()
    readonly property bool isUnc: sharedDirInput.indexOf("\\\\") === 0
                                   || sharedDirInput.indexOf("//") === 0
    readonly property bool isHostMode: sharedDirInput.length > 0 && !isUnc

    // 对端清单里推断出的别名（客户端模式下才有意义）
    readonly property var remoteAliasList: sharedCheckResult && sharedCheckResult.aliases
                                           ? sharedCheckResult.aliases : []
    readonly property bool aliasKnown: remoteAliasList.length === 1
    readonly property string remoteAlias: aliasKnown ? String(remoteAliasList[0]) : ""
    readonly property bool aliasNeedsFix: {
        if (!aliasKnown)
            return false
        for (var i = 0; i < sync.roots.length; ++i)
            if (String(sync.roots[i].alias) !== remoteAlias)
                return true
        return false
    }

    function shareAddress() {
        return localIp.length > 0
                ? "\\\\" + localIp + "\\" + localShareName
                : ""
    }

    // 空别名自动填成对端用的那个（检查时调用）
    function fillEmptyAliases() {
        if (!aliasKnown)
            return
        for (var i = 0; i < sync.roots.length; ++i)
            if (String(sync.roots[i].alias).length === 0)
                sync.setRootAlias(i, remoteAlias)
    }

    // 强制把所有别名统一成对端的（用户点按钮时调用）
    function applyRemoteAlias() {
        if (!aliasKnown)
            return
        for (var i = 0; i < sync.roots.length; ++i)
            sync.setRootAlias(i, remoteAlias)
    }

    function checkColor() {
        if (!sharedCheckResult)
            return mutedColor
        if (!sharedCheckResult.ok)
            return "#e74c3c"
        return sharedCheckResult.hasManifest ? "#2ecc71" : "#f1c40f"
    }

    Component.onCompleted: {
        localIp = sync.localIpv4()
        var d = String(sync.sharedDir).replace(/\\/g, "/")
        var parts = d.split("/").filter(function (p) { return p.length > 0 })
        if (parts.length > 0) {
            var last = parts[parts.length - 1]
            if (last.length > 0 && last.indexOf(":") < 0)
                localShareName = last
        }
    }

    Connections {
        target: sync
        function onSyncFinished(uploaded, downloaded, conflicts) {
            sharedCheckResult = sync.checkShared()
        }
    }

    ScrollView {
        anchors.fill: parent
        clip: true
        ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

        ColumnLayout {
            width: syncPage.width - 16
            spacing: 14

            Text {
                text: "设备同步"
                color: syncPage.textColor
                font.pixelSize: Style.titleSize
                font.bold: true
            }

            // ---- 状态条 ----
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: statusColumn.implicitHeight + 20
                radius: Style.radius
                color: syncPage.panelColor
                border.color: syncPage.borderColor

                ColumnLayout {
                    id: statusColumn
                    anchors.fill: parent
                    anchors.margins: 10
                    spacing: 6

                    Text {
                        text: sync.statusText
                        color: syncPage.textColor
                        font.pixelSize: Style.bodySize
                        wrapMode: Text.WordWrap
                        Layout.fillWidth: true
                    }

                    RowLayout {
                        spacing: 18

                        Text {
                            text: "通道：" + sync.activeChannel
                            color: syncPage.mutedColor
                            font.pixelSize: Style.captionSize
                        }
                        Text {
                            text: "对端：" + (sync.peerName.length > 0 ? sync.peerName : "—")
                            color: syncPage.mutedColor
                            font.pixelSize: Style.captionSize
                        }
                        Text {
                            text: "本机：" + sync.deviceName
                            color: syncPage.mutedColor
                            font.pixelSize: Style.captionSize
                        }
                    }

                    ProgressBar {
                        Layout.fillWidth: true
                        from: 0
                        to: 100
                        value: sync.progress
                        visible: sync.busy
                    }
                }
            }

            // ---- 连接 ----
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: connectColumn.implicitHeight + 24
                radius: Style.radius
                color: syncPage.panelColor
                border.color: syncPage.borderColor

                ColumnLayout {
                    id: connectColumn
                    anchors.fill: parent
                    anchors.margins: 12
                    spacing: 10

                    Text {
                        text: syncPage.isHostMode ? "本机共享给别人（主机）"
                              : (syncPage.sharedDirInput.length > 0 ? "连接别人的共享（客户端）" : "连接")
                        color: syncPage.mutedColor
                        font.pixelSize: 12
                    }

                    Text {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        color: syncPage.mutedColor
                        font.pixelSize: Style.captionSize
                        text: syncPage.isHostMode
                              ? "这台机器当主机：把本地文件夹共享出去，再把下面「让对方连我」的地址发给对方。"
                              : (syncPage.sharedDirInput.length > 0
                                 ? "这台机器当客户端：填对方发给你的地址，填完点「检查」确认能连上。"
                                 : "先决定这台机器的角色：填本地路径 = 我共享给别人；填 \\\\对方IP\\共享名 = 我连别人的共享。")
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8

                        Text {
                            text: syncPage.isHostMode ? "本机共享目录"
                                  : (syncPage.sharedDirInput.length > 0 ? "对方共享目录" : "共享目录")
                            color: syncPage.mutedColor
                            font.pixelSize: Style.captionSize
                            Layout.preferredWidth: 90
                        }

                        Rectangle {
                            Layout.fillWidth: true
                            Layout.preferredHeight: 34
                            radius: Style.radiusSmall
                            color: "transparent"
                            border.color: sharedDirField.activeFocus ? syncPage.accentColor
                                                                    : syncPage.borderColor

                            TextField {
                                id: sharedDirField
                                anchors.fill: parent
                                anchors.leftMargin: 8
                                anchors.rightMargin: 8
                                text: sync.sharedDir
                                placeholderText: "\\\\对端IP\\共享名，或两台都可见的文件夹（网盘/本地目录）"
                                color: syncPage.textColor
                                placeholderTextColor: syncPage.mutedColor
                                font.pixelSize: Style.bodySize
                                background: null
                                onEditingFinished: sync.setSharedDir(text)
                            }
                        }
                    }

                    RowLayout {
                        spacing: 8

                        Button {
                            text: "检查"
                            enabled: sharedDirField.text.length > 0
                            onClicked: {
                                sync.setSharedDir(sharedDirField.text)
                                sharedCheckResult = sync.checkShared()
                                syncPage.fillEmptyAliases()
                            }
                        }
                        Button {
                            text: "保存"
                            enabled: sharedDirField.text.length > 0
                            onClicked: sync.setSharedDir(sharedDirField.text)
                        }
                        Button {
                            text: "浏览…"
                            onClicked: sharedDirDialog.open()
                        }
                        Button {
                            text: "打开"
                            enabled: sharedDirField.text.length > 0
                            onClicked: sync.openPath(sharedDirField.text)
                        }
                        Button {
                            text: "复制路径"
                            enabled: sharedDirField.text.length > 0
                            onClicked: sync.copyToClipboard(sharedDirField.text)
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8

                        Rectangle {
                            Layout.preferredWidth: 8
                            Layout.preferredHeight: 8
                            Layout.alignment: Qt.AlignVCenter
                            radius: 4
                            color: checkColor()
                        }

                        Text {
                            text: sharedCheckResult ? sharedCheckResult.message
                                                    : "点「检查」看这台机器能不能访问共享目录"
                            color: syncPage.mutedColor
                            font.pixelSize: Style.captionSize
                            wrapMode: Text.WordWrap
                            Layout.fillWidth: true
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8
                        visible: syncPage.isUnc && syncPage.aliasKnown

                        Text {
                            Layout.fillWidth: true
                            wrapMode: Text.WordWrap
                            color: syncPage.aliasNeedsFix ? "#f1c40f" : syncPage.mutedColor
                            font.pixelSize: Style.captionSize
                            text: syncPage.aliasNeedsFix
                                  ? "对端用的别名是「" + syncPage.remoteAlias
                                    + "」，两边不一致会各占一棵子树。"
                                  : "别名已对齐：两边都是「" + syncPage.remoteAlias + "」"
                        }

                        Button {
                            visible: syncPage.aliasNeedsFix
                            text: "改成一致"
                            onClicked: syncPage.applyRemoteAlias()
                        }
                    }

                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 1
                        color: syncPage.borderColor
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8
                        visible: !syncPage.isUnc

                        Text {
                            text: "让对方连我"
                            color: syncPage.mutedColor
                            font.pixelSize: Style.captionSize
                            Layout.preferredWidth: 64
                        }

                        Text {
                            visible: shareAddress().length > 0
                            text: shareAddress()
                            color: syncPage.textColor
                            font.pixelSize: Style.bodySize
                            elide: Text.ElideMiddle
                            Layout.fillWidth: true
                        }
                        Text {
                            visible: shareAddress().length === 0
                            text: "未检测到局域网地址"
                            color: syncPage.mutedColor
                            font.pixelSize: Style.captionSize
                            Layout.fillWidth: true
                        }

                        Rectangle {
                            Layout.preferredWidth: 120
                            Layout.preferredHeight: 34
                            radius: Style.radiusSmall
                            color: "transparent"
                            border.color: shareNameField.activeFocus ? syncPage.accentColor
                                                                     : syncPage.borderColor

                            TextField {
                                id: shareNameField
                                anchors.fill: parent
                                anchors.leftMargin: 8
                                anchors.rightMargin: 8
                                text: syncPage.localShareName
                                placeholderText: "共享名"
                                color: syncPage.textColor
                                placeholderTextColor: syncPage.mutedColor
                                font.pixelSize: Style.captionSize
                                background: null
                                onEditingFinished: syncPage.localShareName = text.trim()
                            }
                        }

                        Button {
                            text: "复制地址"
                            enabled: shareAddress().length > 0
                            onClicked: sync.copyToClipboard(shareAddress())
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 4
                        visible: !syncPage.isUnc

                        Button {
                            text: shareHelp.visible ? "▾ 怎么在本机开一次共享" : "▸ 怎么在本机开一次共享"
                            flat: true
                            onClicked: shareHelp.visible = !shareHelp.visible
                        }
                    }

                    ColumnLayout {
                        id: shareHelp
                        visible: false
                        Layout.fillWidth: true
                        spacing: 6

                        Text {
                            text: "在要共享的那台机器上，用管理员权限运行（路径和共享名按需改）："
                            color: syncPage.mutedColor
                            font.pixelSize: Style.captionSize
                            wrapMode: Text.WordWrap
                            Layout.fillWidth: true
                        }

                        Rectangle {
                            Layout.fillWidth: true
                            Layout.preferredHeight: 56
                            radius: Style.radiusSmall
                            color: Qt.rgba(0, 0, 0, 0.25)

                            TextEdit {
                                id: shareCmd
                                anchors.fill: parent
                                anchors.margins: 8
                                readOnly: true
                                selectByMouse: true
                                wrapMode: TextEdit.WrapAnywhere
                                color: syncPage.textColor
                                font.family: "Consolas"
                                font.pixelSize: Style.captionSize
                                text: "net share " + syncPage.localShareName
                                      + "=E:\\SyncHub /grant:everyone,full"
                            }
                        }

                        RowLayout {
                            spacing: 8
                            Button {
                                text: "复制命令"
                                onClicked: sync.copyToClipboard(shareCmd.text)
                            }
                        }

                        Text {
                            text: "两台设备填同一个共享目录、同一个别名，才能对上。"
                            color: syncPage.mutedColor
                            font.pixelSize: Style.captionSize
                            wrapMode: Text.WordWrap
                            Layout.fillWidth: true
                        }
                    }
                }
            }

            // ---- 同步范围 ----
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: rootsColumn.implicitHeight + 24
                radius: Style.radius
                color: syncPage.panelColor
                border.color: syncPage.borderColor

                ColumnLayout {
                    id: rootsColumn
                    anchors.fill: parent
                    anchors.margins: 12
                    spacing: 8

                    Text {
                        text: "同步范围"
                        color: syncPage.mutedColor
                        font.pixelSize: 12
                    }

                    Text {
                        visible: sync.roots.length === 0
                        text: "还没有要同步的文件夹。点下面的「添加文件夹…」选一个，可以加多个。"
                        color: syncPage.mutedColor
                        font.pixelSize: Style.captionSize
                        wrapMode: Text.WordWrap
                        Layout.fillWidth: true
                    }

                    Text {
                        text: "别名：两台设备必须填同一个，例如都填 share；否则各占一棵子树，永远碰不上。"
                        color: syncPage.mutedColor
                        font.pixelSize: Style.captionSize
                        wrapMode: Text.WordWrap
                        Layout.fillWidth: true
                    }

                    Repeater {
                        model: sync.roots

                        delegate: Rectangle {
                            Layout.fillWidth: true
                            Layout.preferredHeight: 46
                            radius: Style.radiusSmall
                            color: "transparent"
                            border.color: syncPage.borderColor

                            RowLayout {
                                anchors.fill: parent
                                anchors.margins: 8
                                spacing: 8

                                Rectangle {
                                    Layout.preferredWidth: 120
                                    Layout.preferredHeight: 30
                                    radius: Style.radiusSmall
                                    color: "transparent"
                                    border.color: aliasField.activeFocus ? syncPage.accentColor
                                                                         : syncPage.borderColor

                                    TextField {
                                        id: aliasField
                                        anchors.fill: parent
                                        anchors.leftMargin: 6
                                        anchors.rightMargin: 6
                                        text: modelData.alias
                                        placeholderText: "两台必须同名"
                                        color: syncPage.textColor
                                        placeholderTextColor: syncPage.mutedColor
                                        font.pixelSize: Style.captionSize
                                        background: null
                                        onEditingFinished: sync.setRootAlias(modelData.alias, text)
                                    }
                                }

                                Text {
                                    text: modelData.localPath
                                    color: syncPage.mutedColor
                                    font.pixelSize: Style.captionSize
                                    Layout.fillWidth: true
                                    elide: Text.ElideMiddle
                                }

                                Button {
                                    text: "移除"
                                    onClicked: sync.removeRoot(modelData.alias)
                                }
                            }
                        }
                    }

                    RowLayout {
                        spacing: 8
                        Button {
                            text: "添加文件夹…"
                            onClicked: rootDialog.open()
                        }
                    }
                }
            }

            // ---- 同步 ----
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: syncColumn.implicitHeight + 24
                radius: Style.radius
                color: syncPage.panelColor
                border.color: syncPage.borderColor

                ColumnLayout {
                    id: syncColumn
                    anchors.fill: parent
                    anchors.margins: 12
                    spacing: 10

                    Text {
                        text: "同步"
                        color: syncPage.mutedColor
                        font.pixelSize: 12
                    }

                    RowLayout {
                        spacing: 10

                        Button {
                            text: sync.busy ? "同步中…" : "立即同步"
                            enabled: !sync.busy
                            onClicked: sync.syncNow()
                        }

                        CheckBox {
                            text: "文件变化时自动同步"
                            checked: sync.watchFiles
                            onToggled: sync.setWatchFiles(checked)
                        }
                    }

                    RowLayout {
                        spacing: 8

                        Text {
                            text: "定时兜底"
                            color: syncPage.mutedColor
                            font.pixelSize: 12
                        }

                        ComboBox {
                            id: intervalBox
                            Layout.preferredWidth: 120
                            model: ["关闭", "5 分钟", "15 分钟", "60 分钟"]

                            Component.onCompleted: {
                                const mins = sync.intervalMinutes
                                if (mins === 5)        currentIndex = 1
                                else if (mins === 15)  currentIndex = 2
                                else if (mins === 60)  currentIndex = 3
                                else                   currentIndex = 0
                            }

                            onActivated: {
                                const table = [0, 5, 15, 60]
                                sync.setAutoSyncInterval(table[currentIndex])
                            }
                        }

                        Text {
                            text: sync.autoSync ? "已开启" : "关闭"
                            color: syncPage.mutedColor
                            font.pixelSize: Style.captionSize
                        }
                    }
                }
            }

            // ---- 发送文件（折叠，默认收起）----
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: sendColumn.implicitHeight + 20
                radius: Style.radius
                color: syncPage.panelColor
                border.color: dropArea.containsDrag ? syncPage.accentColor : syncPage.borderColor

                DropArea {
                    id: dropArea
                    anchors.fill: parent
                    onDropped: {
                        var files = []
                        for (var i = 0; i < drop.urls.length; ++i)
                            files.push(String(drop.urls[i]))
                        sync.sendFiles(files)
                    }
                }

                ColumnLayout {
                    id: sendColumn
                    anchors.fill: parent
                    anchors.margins: 10
                    spacing: 8

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8

                        Button {
                            text: sendBody.visible ? "▾ 发送文件（拖拽即发）"
                                                   : "▸ 发送文件（拖拽即发）"
                            flat: true
                            onClicked: sendBody.visible = !sendBody.visible
                        }

                        Text {
                            visible: !sendBody.visible
                            text: dropArea.containsDrag ? "松手即发送" : "也可以把文件拖到这张卡片上"
                            color: dropArea.containsDrag ? syncPage.accentColor : syncPage.mutedColor
                            font.pixelSize: Style.captionSize
                            Layout.fillWidth: true
                        }
                    }

                    ColumnLayout {
                        id: sendBody
                        visible: false
                        Layout.fillWidth: true
                        spacing: 8

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 8

                            Button {
                                text: "选择文件…"
                                enabled: !sync.sending
                                onClicked: sendDialog.open()
                            }

                            Text {
                                text: sync.sendStatus.length > 0
                                      ? sync.sendStatus
                                      : "文件会落到共享目录的 _投递/<你的设备名>/ 里；对端程序在线时自动收进收件箱。"
                                color: syncPage.mutedColor
                                font.pixelSize: Style.captionSize
                                wrapMode: Text.WordWrap
                                Layout.fillWidth: true
                            }
                        }

                        ProgressBar {
                            Layout.fillWidth: true
                            indeterminate: true
                            visible: sync.sending
                        }
                    }
                }
            }

            // ---- 冲突列表 ----
            Text {
                text: "冲突记录（" + sync.conflicts.length + "）"
                color: syncPage.mutedColor
                font.pixelSize: 12
            }

            Text {
                visible: sync.conflicts.length === 0
                text: "没有冲突。两边改到同一个文件时，这里会出现记录，"
                      + "被保留的那一份会以「冲突副本」为名留在同一个文件夹里。"
                color: syncPage.mutedColor
                font.pixelSize: Style.captionSize
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
            }

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 6

                Repeater {
                    model: sync.conflicts

                    delegate: Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: conflictColumn.implicitHeight + 16
                        radius: Style.radiusSmall
                        color: syncPage.panelColor
                        border.color: syncPage.borderColor

                        ColumnLayout {
                            id: conflictColumn
                            anchors.fill: parent
                            anchors.margins: 8
                            spacing: 4

                            Text {
                                text: modelData.relPath
                                color: syncPage.textColor
                                font.pixelSize: Style.bodySize
                                font.bold: true
                                wrapMode: Text.WordWrap
                                Layout.fillWidth: true
                            }

                            Text {
                                text: "保留副本：" + modelData.conflictCopy
                                color: syncPage.mutedColor
                                font.pixelSize: Style.captionSize
                                wrapMode: Text.WordWrap
                                Layout.fillWidth: true
                            }

                            RowLayout {
                                spacing: 12

                                Text {
                                    text: modelData.detectedAt
                                    color: syncPage.mutedColor
                                    font.pixelSize: Style.captionSize
                                }
                                Text {
                                    text: "本机 " + modelData.localHash + " / 对端 " + modelData.remoteHash
                                    color: syncPage.mutedColor
                                    font.pixelSize: Style.captionSize
                                }

                                Button {
                                    text: "打开所在文件夹"
                                    onClicked: sync.openConflictFolder(modelData.index)
                                }
                            }
                        }
                    }
                }
            }

            // ---- 本机设备名 ----
            RowLayout {
                spacing: 8

                Text {
                    text: "本机设备名"
                    color: syncPage.mutedColor
                    font.pixelSize: 12
                }

                Rectangle {
                    Layout.preferredWidth: 180
                    Layout.preferredHeight: 34
                    radius: Style.radiusSmall
                    color: syncPage.panelColor
                    border.color: deviceField.activeFocus ? syncPage.accentColor
                                                          : syncPage.borderColor

                    TextField {
                        id: deviceField
                        anchors.fill: parent
                        anchors.leftMargin: 8
                        anchors.rightMargin: 8
                        text: sync.deviceName
                        placeholderText: "这台电脑的名字"
                        color: syncPage.textColor
                        placeholderTextColor: syncPage.mutedColor
                        font.pixelSize: Style.bodySize
                        background: null
                    }
                }

                Button {
                    text: "保存"
                    onClicked: sync.setDeviceName(deviceField.text)
                }
            }

            Text {
                text: "这个名称会写进共享目录的清单，也用作投递区里属于你的文件夹名，"
                      + "对端靠它认出文件是谁发的。两台设备不要用同一个名称。"
                color: syncPage.mutedColor
                font.pixelSize: Style.captionSize
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
            }

            // ---- 收件箱 ----
            RowLayout {
                spacing: 8

                Text {
                    text: "收件箱"
                    color: syncPage.mutedColor
                    font.pixelSize: 12
                }

                Text {
                    text: sync.inboxDir
                    color: syncPage.mutedColor
                    font.pixelSize: Style.captionSize
                    elide: Text.ElideMiddle
                    Layout.fillWidth: true
                }

                Button {
                    text: "打开"
                    onClicked: sync.openInbox()
                }
            }

            Text {
                text: "Assistant v" + appVersion + " · 设备同步"
                color: syncPage.mutedColor
                font.pixelSize: Style.captionSize
                Layout.fillWidth: true
            }
        }
    }

    FolderDialog {
        id: rootDialog
        title: "选择要同步的文件夹"
        onAccepted: sync.addRoot(selectedFolder)
    }

    FolderDialog {
        id: sharedDirDialog
        title: "选择共享目录"
        onAccepted: {
            sharedDirField.text = sync.toLocalPath(selectedFolder.toString())
            sync.setSharedDir(sharedDirField.text)
        }
    }

    FileDialog {
        id: sendDialog
        title: "选择要发送的文件"
        fileMode: FileDialog.OpenFiles
        onAccepted: sync.sendFiles(selectedFiles)
    }
}
