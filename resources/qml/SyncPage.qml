import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import "components"

// 设备同步（2026-11-19 UI 改版）
// 信息层级：状态 → 直连 → 同步范围 → 同步 → 发送 → 冲突 → 设备名。
// 所有 SyncBridge 动作/字段保持原样，只重排视觉。
Rectangle {
    id: syncPage
    color: "transparent"

    property string selfText: ""

    Flickable {
        id: flick
        anchors.fill: parent
        anchors.margins: Style.pagePadding
        clip: true
        contentWidth: width
        contentHeight: contentColumn.implicitHeight
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: AppScrollBar { }

        ColumnLayout {
            id: contentColumn
            width: flick.width
            spacing: 12

            AppSectionHeader {
                Layout.fillWidth: true
                Layout.preferredHeight: 48
                title: "设备同步"
                subtitle: "直连 · 6 位码配对 · 只传差异"

                AppButton {
                    text: "自检"
                    icon: "shield"
                    variant: "ghost"
                    small: true
                    onClicked: syncPage.selfText = sync.selfCheck()
                }
            }

            // ---- 状态 ----
            AppCard {
                Layout.fillWidth: true

                ColumnLayout {
                    width: parent.width
                    spacing: 8

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8

                        Rectangle {
                            width: 7
                            height: 7
                            radius: 3.5
                            color: sync.busy ? Style.warning : Style.success
                        }

                        Text {
                            Layout.fillWidth: true
                            text: sync.statusText
                            color: Style.text
                            font.pixelSize: Style.fontBody
                            font.family: Style.fontFamily
                            wrapMode: Text.WordWrap
                        }

                        Text {
                            text: sync.activeChannel
                            color: Style.text3
                            font.pixelSize: 10
                            font.family: Style.fontFamily
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 16

                        Text {
                            text: "通道：" + sync.activeChannel
                            color: Style.text3
                            font.pixelSize: 10
                            font.family: Style.fontFamily
                        }
                        Text {
                            text: "对端：" + (sync.peerName.length > 0 ? sync.peerName : "—")
                            color: Style.text3
                            font.pixelSize: 10
                            font.family: Style.fontFamily
                        }
                        Text {
                            text: "本机：" + sync.deviceName
                            color: Style.text3
                            font.pixelSize: 10
                            font.family: Style.fontFamily
                        }
                        Item { Layout.fillWidth: true }
                    }

                    ProgressBar {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 4
                        from: 0
                        to: 100
                        value: sync.progress
                        visible: sync.busy
                    }
                }
            }

            Timer {
                interval: 3000
                running: true
                repeat: true
                onTriggered: sync.refreshPairState()
            }

            // ---- 直连 ----
            AppCard {
                Layout.fillWidth: true

                ColumnLayout {
                    width: parent.width
                    spacing: 10

                    RowLayout {
                        spacing: 8
                        Text {
                            text: "直连"
                            color: Style.text
                            font.pixelSize: Style.fontSubtitle
                            font.weight: Font.DemiBold
                            font.family: Style.fontFamily
                        }
                        Text {
                            text: "不经过共享目录，主机开端口，连接方拿 6 位码配对"
                            color: Style.text3
                            font.pixelSize: 10
                            font.family: Style.fontFamily
                            Layout.fillWidth: true
                            elide: Text.ElideRight
                        }
                    }

                    Text {
                        Layout.fillWidth: true
                        text: "走 ASYN v2 over TLS，只有主机出示证书，客户端靠口令认证。"
                              + "配对后由连接方点「立即同步」：两边按「本地 / 上次基线 / 对端清单」比对，只传差异、双向生效。主机等对方来拉即可。"
                        color: Style.text3
                        font.pixelSize: 10
                        font.family: Style.fontFamily
                        wrapMode: Text.WordWrap
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 2

                        Text {
                            text: sync.netRunning
                                  ? "本机地址（对方填哪个都行，选两边都通的那个）"
                                  : "本机地址：未监听（点「开始监听」）"
                            color: Style.text3
                            font.pixelSize: 10
                            font.family: Style.fontFamily
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
                                    color: Style.text3
                                    font.pixelSize: 10
                                    font.family: Style.fontFamily
                                    Layout.preferredWidth: 150
                                    elide: Text.ElideRight
                                }
                                Text {
                                    Layout.fillWidth: true
                                    text: modelData.address
                                          + (modelData.hint.length > 0 ? "  （" + modelData.hint + "）" : "")
                                    color: modelData.hint.length > 0 ? Style.text3 : Style.text
                                    font.pixelSize: Style.fontBody
                                    font.family: Style.fontFamily
                                    elide: Text.ElideRight
                                }
                                AppIconButton {
                                    icon: "copy"
                                    iconSize: 12
                                    tooltip: "复制地址"
                                    onClicked: sync.copyToClipboard(modelData.address)
                                }
                            }
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 6

                        AppButton {
                            text: "放行防火墙"
                            variant: "secondary"
                            small: true
                            onClicked: sync.openFirewallPort()
                        }
                        AppButton {
                            text: sync.netRunning ? "停止监听" : "开始监听"
                            variant: sync.netRunning ? "danger" : "primary"
                            small: true
                            onClicked: {
                                if (sync.netRunning)
                                    sync.stopNetServer()
                                else
                                    sync.startNetServer()
                            }
                        }
                        AppButton {
                            text: sync.pairActive ? "关闭配对窗口" : "生成配对码"
                            variant: "secondary"
                            small: true
                            enabled: sync.netRunning
                            onClicked: {
                                if (sync.pairActive)
                                    sync.stopPairing()
                                else
                                    sync.startPairing()
                            }
                        }
                        Item { Layout.fillWidth: true }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 10

                        Text {
                            visible: sync.pairActive
                            text: sync.pairCode
                            color: Style.accent
                            font.pixelSize: 26
                            font.weight: Font.DemiBold
                            font.letterSpacing: 4
                            font.family: "Consolas"
                        }

                        Text {
                            Layout.fillWidth: true
                            text: String(sync.netStatus)
                            color: sync.pairActive ? Style.accent : Style.text3
                            font.pixelSize: sync.pairActive ? Style.fontBody : 9.5
                            font.family: Style.fontFamily
                            wrapMode: Text.WordWrap
                        }
                    }

                    Repeater {
                        model: sync.pairedDevices

                        delegate: RowLayout {
                            Layout.fillWidth: true
                            spacing: 8

                            Text {
                                Layout.fillWidth: true
                                color: Style.text2
                                font.pixelSize: 10
                                font.family: Style.fontFamily
                                elide: Text.ElideMiddle
                                text: modelData.deviceId + " · "
                                      + String(modelData.fingerprint).substring(0, 16)
                            }

                            AppButton {
                                text: "解除"
                                variant: "ghost"
                                small: true
                                onClicked: sync.forgetDevice(modelData.deviceId)
                            }
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 6

                        AppTextField {
                            id: peerField
                            Layout.fillWidth: true
                            compact: true
                            placeholder: "主机地址，例如 100.116.234.107:45871"
                        }

                        AppTextField {
                            id: codeField
                            Layout.preferredWidth: 96
                            compact: true
                            placeholder: "6 位码"
                            maximumLength: 6
                            inputMethodHints: Qt.ImhDigitsOnly
                        }

                        AppButton {
                            text: "配对"
                            variant: "primary"
                            small: true
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
                            text: "已与主机配对；每轮同步会同时挂载直连通道。"
                            color: Style.text3
                            font.pixelSize: 10
                            font.family: Style.fontFamily
                        }

                        AppButton {
                            text: "解除配对"
                            variant: "ghost"
                            small: true
                            onClicked: sync.unpairPeer()
                        }
                    }

                    Text {
                        visible: syncPage.selfText.length > 0
                        Layout.fillWidth: true
                        text: syncPage.selfText
                        color: Style.text2
                        font.pixelSize: 10
                        font.family: Style.fontFamily
                        lineHeight: 1.35
                        wrapMode: Text.WordWrap
                    }
                }
            }

            // ---- 同步范围 ----
            AppCard {
                Layout.fillWidth: true

                ColumnLayout {
                    width: parent.width
                    spacing: 8

                    Text {
                        text: "同步范围"
                        color: Style.text
                        font.pixelSize: Style.fontSubtitle
                        font.weight: Font.DemiBold
                        font.family: Style.fontFamily
                    }

                    Text {
                        visible: sync.roots.length === 0
                        text: "还没有要同步的文件夹。点下面的「添加文件夹…」选一个，可以加多个。"
                        color: Style.text3
                        font.pixelSize: 10
                        font.family: Style.fontFamily
                        wrapMode: Text.WordWrap
                        Layout.fillWidth: true
                    }

                    Repeater {
                        model: sync.roots

                        delegate: RowLayout {
                            Layout.fillWidth: true
                            spacing: 8

                            AppTextField {
                                Layout.preferredWidth: 120
                                compact: true
                                text: modelData.alias
                                placeholder: "两台必须同名"
                                onEditingFinished: sync.setRootAlias(modelData.alias, text)
                            }

                            Text {
                                Layout.fillWidth: true
                                text: modelData.localPath
                                color: Style.text2
                                font.pixelSize: 10
                                font.family: Style.fontFamily
                                elide: Text.ElideMiddle
                            }

                            AppIconButton {
                                icon: "trash"
                                iconSize: 13
                                danger: true
                                tooltip: "移除"
                                onClicked: sync.removeRoot(modelData.alias)
                            }
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8

                        AppButton {
                            text: "添加文件夹…"
                            icon: "plus"
                            variant: "secondary"
                            small: true
                            onClicked: rootDialog.open()
                        }
                        Item { Layout.fillWidth: true }
                    }

                    Text {
                        text: "别名：两台设备必须填同一个，例如都填 share；否则各占一棵子树，永远碰不上。"
                        color: Style.text3
                        font.pixelSize: 10
                        font.family: Style.fontFamily
                        wrapMode: Text.WordWrap
                        Layout.fillWidth: true
                    }
                }
            }

            // ---- 同步 ----
            AppCard {
                Layout.fillWidth: true

                ColumnLayout {
                    width: parent.width
                    spacing: 10

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8

                        Text {
                            text: "同步"
                            color: Style.text
                            font.pixelSize: Style.fontSubtitle
                            font.weight: Font.DemiBold
                            font.family: Style.fontFamily
                        }
                        Item { Layout.fillWidth: true }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 10

                        AppButton {
                            text: sync.busy ? "同步中…" : "立即同步"
                            icon: "sync"
                            variant: "primary"
                            enabled: !sync.busy && sync.peerPaired
                            onClicked: sync.syncNow()
                        }

                        AppSwitch {
                            checked: sync.watchFiles
                            onToggled: sync.setWatchFiles(checked)
                        }
                        Text {
                            text: "文件变化时自动同步"
                            color: Style.text2
                            font.pixelSize: 10
                            font.family: Style.fontFamily
                        }
                        Item { Layout.fillWidth: true }
                    }

                    Text {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        color: Style.text3
                        font.pixelSize: 10
                        font.family: Style.fontFamily
                        visible: !sync.peerPaired && sync.pairedDevices.length > 0
                        text: "你是主机：文件在你这边，等对方点「立即同步」来拉取；这台机器上的「立即同步」没有对端可连，所以是灰的。"
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8

                        Text {
                            text: "定时兜底"
                            color: Style.text2
                            font.pixelSize: 10
                            font.family: Style.fontFamily
                        }

                        AppComboBox {
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
                            color: Style.text3
                            font.pixelSize: 10
                            font.family: Style.fontFamily
                        }
                        Item { Layout.fillWidth: true }
                    }
                }
            }

            // ---- 发送文件 ----
            AppCard {
                Layout.fillWidth: true

                DropArea {
                    id: dropArea
                    anchors.fill: parent
                    onDropped: {
                        var files = []
                        for (var i = 0; i < drop.urls.length; ++i)
                            files.push(String(drop.urls[i]))
                        sync.queueSend(files)
                    }
                }

                ColumnLayout {
                    width: parent.width
                    spacing: 8

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8

                        Text {
                            text: "发送文件"
                            color: Style.text
                            font.pixelSize: Style.fontSubtitle
                            font.weight: Font.DemiBold
                            font.family: Style.fontFamily
                        }

                        Text {
                            Layout.fillWidth: true
                            text: dropArea.containsDrag ? "松手即发送"
                                                       : "直接落进对端收件箱"
                            color: dropArea.containsDrag ? Style.accent : Style.text3
                            font.pixelSize: 10
                            font.family: Style.fontFamily
                            elide: Text.ElideRight
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 6

                        AppButton {
                            text: "选择文件…"
                            icon: "plus"
                            variant: "secondary"
                            small: true
                            onClicked: sendDialog.open()
                        }
                        AppButton {
                            text: sync.outboxCount > 0 ? "立即发送（" + sync.outboxCount + "）" : "立即发送"
                            icon: "send"
                            variant: "primary"
                            small: true
                            enabled: sync.outboxCount > 0 && !sync.sending
                            onClicked: sync.sendQueuedNow()
                        }
                        AppButton {
                            text: "打开收件箱"
                            variant: "ghost"
                            small: true
                            onClicked: sync.openInbox()
                        }
                        Item { Layout.fillWidth: true }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8

                        Text {
                            text: "收件箱"
                            color: Style.text3
                            font.pixelSize: 10
                            font.family: Style.fontFamily
                        }
                        Text {
                            Layout.fillWidth: true
                            text: sync.inboxDir
                            color: Style.text2
                            font.pixelSize: 10
                            font.family: Style.fontFamily
                            elide: Text.ElideMiddle
                        }
                        AppButton {
                            text: "更换…"
                            variant: "ghost"
                            small: true
                            onClicked: inboxDialog.open()
                        }
                        AppButton {
                            text: "默认"
                            variant: "ghost"
                            small: true
                            onClicked: sync.resetInboxDir()
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8

                        Text {
                            Layout.fillWidth: true
                            text: sync.pairStatus
                            color: sync.peerPaired ? Style.text3 : Style.accent
                            font.pixelSize: 10
                            font.family: Style.fontFamily
                            wrapMode: Text.WordWrap
                        }

                        AppIconButton {
                            icon: "refresh"
                            iconSize: 12
                            tooltip: "刷新配对"
                            onClicked: sync.refreshPairState()
                        }
                    }

                    Text {
                        visible: sync.outboxCount === 0
                        text: "队列是空的。拖文件到这张卡片上，或点「选择文件…」。"
                        color: Style.text3
                        font.pixelSize: 10
                        font.family: Style.fontFamily
                        wrapMode: Text.WordWrap
                        Layout.fillWidth: true
                    }

                    Repeater {
                        model: sync.outboxList

                        delegate: RowLayout {
                            Layout.fillWidth: true
                            spacing: 8

                            Text {
                                text: modelData.name
                                color: Style.text
                                font.pixelSize: 10
                                font.family: Style.fontFamily
                                elide: Text.ElideMiddle
                                Layout.fillWidth: true
                            }

                            Text {
                                text: modelData.lastError.length > 0
                                      ? "重试 " + modelData.attempts + " 次：" + modelData.lastError
                                      : (modelData.attempts > 0
                                         ? "已重试 " + modelData.attempts + " 次"
                                         : Math.round(modelData.size / 1024) + " KB")
                                color: modelData.lastError.length > 0 ? Style.danger : Style.text3
                                font.pixelSize: 10
                                font.family: Style.fontFamily
                                elide: Text.ElideRight
                                Layout.preferredWidth: 240
                            }

                            AppIconButton {
                                icon: "close"
                                iconSize: 11
                                tooltip: "移除"
                                onClicked: sync.removeQueuedItem(modelData.id)
                            }
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8

                        ProgressBar {
                            Layout.fillWidth: true
                            Layout.preferredHeight: 4
                            indeterminate: true
                            visible: sync.sending
                        }

                        Text {
                            text: String(sync.sendStatus)
                            visible: sync.sendStatus.length > 0 && !sync.sending
                            color: Style.text3
                            font.pixelSize: 10
                            font.family: Style.fontFamily
                            elide: Text.ElideRight
                            Layout.fillWidth: true
                        }
                    }

                    Rectangle {
                        visible: dropArea.containsDrag
                        Layout.fillWidth: true
                        Layout.preferredHeight: 1
                        color: Style.accent
                    }
                }
            }

            // ---- 冲突记录 ----
            AppCard {
                Layout.fillWidth: true

                ColumnLayout {
                    width: parent.width
                    spacing: 8

                    Text {
                        text: "冲突记录（" + sync.conflicts.length + "）"
                        color: Style.text
                        font.pixelSize: Style.fontSubtitle
                        font.weight: Font.DemiBold
                        font.family: Style.fontFamily
                    }

                    Text {
                        visible: sync.conflicts.length === 0
                        text: "没有冲突。两边改到同一个文件时，这里会出现记录，被保留的那一份会以「冲突副本」为名留在同一个文件夹里。"
                        color: Style.text3
                        font.pixelSize: 10
                        font.family: Style.fontFamily
                        wrapMode: Text.WordWrap
                        Layout.fillWidth: true
                    }

                    Repeater {
                        model: sync.conflicts

                        delegate: Rectangle {
                            Layout.fillWidth: true
                            implicitHeight: conflictColumn.implicitHeight + 16
                            radius: Style.radiusControl
                            color: Style.surface2
                            border.width: 1
                            border.color: Style.line

                            ColumnLayout {
                                id: conflictColumn
                                anchors.fill: parent
                                anchors.margins: 8
                                spacing: 3

                                Text {
                                    text: modelData.relPath
                                    color: Style.text
                                    font.pixelSize: Style.fontBody
                                    font.weight: Font.DemiBold
                                    font.family: Style.fontFamily
                                    wrapMode: Text.WordWrap
                                    Layout.fillWidth: true
                                }
                                Text {
                                    text: "保留副本：" + modelData.conflictCopy
                                    color: Style.text3
                                    font.pixelSize: 10
                                    font.family: Style.fontFamily
                                    wrapMode: Text.WordWrap
                                    Layout.fillWidth: true
                                }
                                RowLayout {
                                    spacing: 12
                                    Text {
                                        text: modelData.detectedAt
                                        color: Style.text3
                                        font.pixelSize: 10
                                        font.family: Style.fontFamily
                                    }
                                    AppButton {
                                        text: "打开所在文件夹"
                                        variant: "ghost"
                                        small: true
                                        onClicked: sync.openConflictFolder(modelData.index)
                                    }
                                }
                            }
                        }
                    }
                }
            }

            // ---- 设备名 ----
            AppCard {
                Layout.fillWidth: true

                ColumnLayout {
                    width: parent.width
                    spacing: 8

                    Text {
                        text: "本机设备名"
                        color: Style.text
                        font.pixelSize: Style.fontSubtitle
                        font.weight: Font.DemiBold
                        font.family: Style.fontFamily
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 6

                        AppTextField {
                            id: deviceField
                            Layout.preferredWidth: 220
                            text: sync.deviceName
                            placeholder: "这台电脑的名字"
                        }
                        AppButton {
                            text: "保存"
                            variant: "primary"
                            small: true
                            onClicked: sync.setDeviceName(deviceField.text)
                        }
                        Item { Layout.fillWidth: true }
                    }

                    Text {
                        text: "配对时主机看到的就是这个名称，两台设备不要用同一个。"
                        color: Style.text3
                        font.pixelSize: 10
                        font.family: Style.fontFamily
                        wrapMode: Text.WordWrap
                        Layout.fillWidth: true
                    }
                }
            }

            Text {
                text: "Assistant v" + appVersion + " · 设备同步"
                color: Style.text3
                font.pixelSize: 10
                font.family: Style.fontFamily
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
        id: inboxDialog
        title: "选择收件箱文件夹"
        onAccepted: sync.setInboxDir(selectedFolder)
    }

    FileDialog {
        id: sendDialog
        title: "选择要发送的文件"
        fileMode: FileDialog.OpenFiles
        onAccepted: sync.queueSend(selectedFiles)
    }
}
