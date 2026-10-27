import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts

// 设备同步页（Phase: 联网功能）
// 顶部状态条 / 路径配置 / 立即同步 / 自动同步 / 冲突列表
// 主题色统一走 Style 单例。C++ 侧由 SyncBridge 注入为 `sync`。
Rectangle {
    id: syncPage
    color: "transparent"

    readonly property color textColor:  Style.textColor
    readonly property color mutedColor: Style.mutedColor
    readonly property color accentColor: Style.accentColor
    readonly property color borderColor: Style.borderColor
    readonly property color panelColor: Style.panelColor

    function sectionLabel(t) {
        return t
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
                            text: "对端：" + sync.peerName
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

            // ---- 发送文件（拖拽即发）----
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: dropColumn.implicitHeight + 24
                radius: Style.radius
                color: dropArea.containsDrag ? Qt.rgba(0.29, 0.56, 0.85, 0.16)
                                             : syncPage.panelColor
                border.color: dropArea.containsDrag ? syncPage.accentColor
                                                    : syncPage.borderColor
                border.width: dropArea.containsDrag ? 2 : 1

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
                    id: dropColumn
                    anchors.fill: parent
                    anchors.margins: 12
                    spacing: 8

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8

                        Text {
                            text: "把文件拖到这里发送"
                            color: syncPage.textColor
                            font.pixelSize: Style.bodySize
                            font.bold: true
                            Layout.fillWidth: true
                        }

                        Button {
                            text: "选择文件…"
                            enabled: !sync.sending
                            onClicked: sendDialog.open()
                        }
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

                    ProgressBar {
                        Layout.fillWidth: true
                        indeterminate: true
                        visible: sync.sending
                    }
                }
            }

            // ---- 同步范围：要同步的本地文件夹（可以多个）----
            Text {
                text: "同步范围"
                color: syncPage.mutedColor
                font.pixelSize: 12
            }

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 6

                Text {
                    visible: sync.roots.length === 0
                    text: "还没有要同步的文件夹。点下面的「添加文件夹…」选一个，可以加多个。"
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
                        color: syncPage.panelColor
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
                                    placeholderText: "别名"
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

                Button {
                    text: "添加文件夹…"
                    onClicked: rootDialog.open()
                }
            }

            // ---- 中转站 ----
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 6

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8

                    Text {
                        text: "共享目录"
                        color: syncPage.mutedColor
                        font.pixelSize: Style.captionSize
                        Layout.preferredWidth: 76
                    }

                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 34
                        radius: Style.radiusSmall
                        color: syncPage.panelColor
                        border.color: sharedDirField.activeFocus ? syncPage.accentColor
                                                                 : syncPage.borderColor

                        TextField {
                            id: sharedDirField
                            anchors.fill: parent
                            anchors.leftMargin: 8
                            anchors.rightMargin: 8
                            text: sync.sharedDir
                            placeholderText: "两台电脑都能看到的中转目录（可放在网盘里）"
                            color: syncPage.textColor
                            placeholderTextColor: syncPage.mutedColor
                            font.pixelSize: Style.bodySize
                            background: null
                        }
                    }

                    Button {
                        text: "浏览…"
                        onClicked: sharedDirDialog.open()
                    }
                }

                Text {
                    text: "提示：每个本地文件夹在共享目录里各占一层，层名就是上面的「别名」，"
                          + "所以两个都叫「文档」的文件夹也不会互相覆盖。"
                          + "共享目录也可以直接填 UNC 路径，例如 "
                          + "\\\\192.168.1.5\\Assistant（对方用管理员权限开一次文件夹共享即可："
                          + "net share Assistant=E:\\要共享的目录 /grant:everyone,full）。"
                    color: syncPage.mutedColor
                    font.pixelSize: Style.captionSize
                    wrapMode: Text.WordWrap
                    Layout.fillWidth: true
                }

                Text {
                    id: sharedCheck
                    text: ""
                    visible: text.length > 0
                    color: syncPage.mutedColor
                    font.pixelSize: Style.captionSize
                    wrapMode: Text.WordWrap
                    Layout.fillWidth: true
                }
            }

            // ---- 操作 ----
            RowLayout {
                spacing: 8

                Button {
                    text: sync.busy ? "同步中…" : "立即同步"
                    enabled: !sync.busy
                    onClicked: {
                        sync.setSharedDir(sharedDirField.text)
                        sync.syncNow()
                    }
                }

                Button {
                    text: "保存共享目录"
                    onClicked: {
                        sync.setSharedDir(sharedDirField.text)
                        sharedCheck.text = ""
                    }
                }

                Button {
                    text: "检查共享"
                    enabled: sharedDirField.text.length > 0
                    onClicked: {
                        const r = sync.checkShared()
                        sharedCheck.text = r.ok ? r.message : ("不可用：" + r.message)
                    }
                }

                Button {
                    text: "打开共享目录"
                    enabled: sharedDirField.text.length > 0
                    onClicked: sync.openPath(sharedDirField.text)
                }
            }

            // ---- 自动同步 ----
            RowLayout {
                spacing: 8

                Text {
                    text: "自动同步"
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

                CheckBox {
                    text: "文件变化时自动同步"
                    checked: sync.watchFiles
                    onToggled: sync.setWatchFiles(checked)
                }

                Text {
                    text: sync.autoSync ? "定时兜底已开启" : "仅变化触发"
                    color: syncPage.mutedColor
                    font.pixelSize: Style.captionSize
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
            sharedCheck.text = ""
        }
    }

    FileDialog {
        id: sendDialog
        title: "选择要发送的文件"
        fileMode: FileDialog.OpenFiles
        onAccepted: sync.sendFiles(selectedFiles)
    }
}
