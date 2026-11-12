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

    // 自检结果（点「自检」后填充，多行）
    property string selfText: ""

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

            // ---- 直连（ASYN v2）----
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: directColumn.implicitHeight + 20
                radius: Style.radius
                color: syncPage.panelColor
                border.color: syncPage.borderColor

                ColumnLayout {
                    id: directColumn
                    anchors.fill: parent
                    anchors.margins: 10
                    spacing: 8

                    Text {
                        text: "直连"
                        color: syncPage.textColor
                        font.pixelSize: Style.bodySize
                        font.bold: true
                    }

                    Text {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        color: syncPage.mutedColor
                        font.pixelSize: Style.captionSize
                        text: "不经过共享目录：主机开一个端口，连接方拿 6 位码配对。"
                              + "走 ASYN v2 over TLS，只有主机出示证书，客户端靠口令认证。\n"
                              + "配对后点下面「立即同步」：两边按「本地 / 上次基线 / 对端清单」比对，"
                              + "只传差异、双向生效，文件直接走这条 TLS 连接，不经过任何中转。"
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 2

                        Text {
                            text: sync.netRunning
                                  ? "本机地址（对方填哪个都行，选两边都通的那个）"
                                  : "本机地址：未监听（点「开始监听」）"
                            color: syncPage.mutedColor
                            font.pixelSize: Style.captionSize
                            wrapMode: Text.WordWrap
                            Layout.fillWidth: true
                        }

                        Repeater {
                            model: sync.netAddresses

                            delegate: RowLayout {
                                Layout.fillWidth: true
                                spacing: 8

                                Text {
                                    text: modelData.name + " · " + modelData.kind
                                    color: syncPage.mutedColor
                                    font.pixelSize: Style.captionSize
                                    Layout.preferredWidth: 150
                                    elide: Text.ElideRight
                                }
                                Text {
                                    Layout.fillWidth: true
                                    text: modelData.address
                                          + (modelData.hint.length > 0 ? "  （" + modelData.hint + "）" : "")
                                    color: modelData.hint.length > 0 ? syncPage.mutedColor : syncPage.textColor
                                    font.pixelSize: Style.bodySize
                                    elide: Text.ElideRight
                                }
                                Button {
                                    text: "复制"
                                    onClicked: sync.copyToClipboard(modelData.address)
                                }
                            }
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8

                        Button {
                            text: "放行防火墙"
                            onClicked: sync.openFirewallPort()
                        }

                        Button {
                            text: "自检"
                            onClicked: syncPage.selfText = sync.selfCheck()
                        }

                        Text {
                            Layout.fillWidth: true
                            text: "主机模式建议先放行；自检会检查监听 / 地址 / 防火墙 / IPv6 / 配对"
                            color: syncPage.mutedColor
                            font.pixelSize: Style.captionSize
                            wrapMode: Text.WordWrap
                        }
                    }

                    Text {
                        visible: syncPage.selfText.length > 0
                        Layout.fillWidth: true
                        text: syncPage.selfText
                        color: syncPage.textColor
                        font.pixelSize: Style.captionSize
                        lineHeight: 1.3
                        wrapMode: Text.WordWrap
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8

                        Button {
                            text: sync.netRunning ? "停止监听" : "开始监听"
                            onClicked: {
                                if (sync.netRunning)
                                    sync.stopNetServer()
                                else
                                    sync.startNetServer()
                            }
                        }

                        Button {
                            text: sync.pairActive ? "关闭配对窗口" : "生成配对码"
                            enabled: sync.netRunning
                            onClicked: {
                                if (sync.pairActive)
                                    sync.stopPairing()
                                else
                                    sync.startPairing()
                            }
                        }

                        Text {
                            Layout.fillWidth: true
                            wrapMode: Text.WordWrap
                            color: sync.pairActive ? syncPage.accentColor : syncPage.mutedColor
                            font.pixelSize: sync.pairActive ? 22 : Style.captionSize
                            font.bold: sync.pairActive
                            elide: Text.ElideRight
                            text: sync.pairActive ? sync.pairCode : String(sync.netStatus)
                        }
                    }

                    Repeater {
                        model: sync.pairedDevices

                        delegate: RowLayout {
                            Layout.fillWidth: true
                            spacing: 8

                            Text {
                                Layout.fillWidth: true
                                color: syncPage.mutedColor
                                font.pixelSize: Style.captionSize
                                elide: Text.ElideMiddle
                                text: modelData.deviceId + " · "
                                      + String(modelData.fingerprint).substring(0, 16)
                            }

                            Button {
                                text: "解除"
                                onClicked: sync.forgetDevice(modelData.deviceId)
                            }
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8

                        Rectangle {
                            Layout.fillWidth: true
                            Layout.preferredHeight: 32
                            radius: Style.radiusSmall
                            color: "transparent"
                            border.color: peerField.activeFocus ? syncPage.accentColor : syncPage.borderColor

                            TextField {
                                id: peerField
                                anchors.fill: parent
                                anchors.leftMargin: 8
                                anchors.rightMargin: 8
                                placeholderText: "主机地址，例如 100.116.234.107:45871"
                                color: syncPage.textColor
                                placeholderTextColor: syncPage.mutedColor
                                font.pixelSize: Style.captionSize
                                background: null
                            }
                        }

                        Rectangle {
                            Layout.preferredWidth: 96
                            Layout.preferredHeight: 32
                            radius: Style.radiusSmall
                            color: "transparent"
                            border.color: codeField.activeFocus ? syncPage.accentColor : syncPage.borderColor

                            TextField {
                                id: codeField
                                anchors.fill: parent
                                anchors.leftMargin: 8
                                anchors.rightMargin: 8
                                placeholderText: "6 位码"
                                maximumLength: 6
                                color: syncPage.textColor
                                placeholderTextColor: syncPage.mutedColor
                                font.pixelSize: Style.captionSize
                                background: null
                            }
                        }

                        Button {
                            text: "配对"
                            enabled: peerField.text.length > 0 && codeField.text.length === 6
                            onClicked: sync.pairWithPeer(peerField.text, codeField.text)
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8
                        visible: sync.peerPaired

                        Text {
                            Layout.fillWidth: true
                            wrapMode: Text.WordWrap
                            color: syncPage.mutedColor
                            font.pixelSize: Style.captionSize
                            text: "已与主机配对；每轮同步会同时挂载直连通道。"
                        }

                        Button {
                            text: "解除配对"
                            onClicked: sync.unpairPeer()
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
            // ---- 发送：仍走共享目录的 _投递/（旧通道），直连直发在 B 阶段 ----
            Rectangle {
                visible: false
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
                text: "配对时主机看到的就是这个名称，两台设备不要用同一个。"
                color: syncPage.mutedColor
                font.pixelSize: Style.captionSize
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
            }

            // ---- 收件箱：仍绑共享目录的 _投递/，直连版投递（B 阶段）做完前先藏起来 ----
            RowLayout {
                visible: false
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

    FileDialog {
        id: sendDialog
        title: "选择要发送的文件"
        fileMode: FileDialog.OpenFiles
        onAccepted: sync.sendFiles(selectedFiles)
    }
}
