import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "components"

// 最近下载 / 收件箱（2026-11-19 UI 改版）
// 数据与动作全部沿用 InboxBridge；只重排视觉：类型块、状态点、hover 操作、统一菜单。
Rectangle {
    id: downloadsPage
    color: "transparent"

    property string filterKey: "inbox"
    property var rows: []

    readonly property var filters: [
        { "key": "inbox", "label": "待整理" },
        { "key": "filed", "label": "已移出" },
        { "key": "all",   "label": "全部" }
    ]

    function reload() {
        rows = inbox.filterItems(filterKey)
    }

    function stateColor(item) {
        if (item.failed) return Style.danger
        if (item.inBox) return Style.accent
        return Style.text3
    }

    function stateLabel(item) {
        if (item.failed) return "失败"
        if (item.inBox) return "待整理"
        return item.stateText
    }

    Connections {
        target: inbox
        function onItemsChanged() { downloadsPage.reload() }
    }

    Component.onCompleted: reload()

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: Style.pagePadding
        spacing: Style.spacingM

        AppSectionHeader {
            Layout.fillWidth: true
            Layout.preferredHeight: 48
            title: "最近下载"
            subtitle: inbox.pendingCount + " 个待整理 · 不删除源文件，失败保持原位"

            AppButton {
                text: "刷新"
                icon: "refresh"
                variant: "ghost"
                small: true
                onClicked: inbox.refresh()
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 6

            Repeater {
                model: downloadsPage.filters
                delegate: AppChip {
                    text: modelData.label
                    selected: downloadsPage.filterKey === modelData.key
                    onClicked: {
                        downloadsPage.filterKey = modelData.key
                        downloadsPage.reload()
                    }
                }
            }

            Item { Layout.fillWidth: true }
        }

        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true

            ListView {
                id: list
                anchors.fill: parent
                clip: true
                spacing: 6
                model: downloadsPage.rows
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: AppScrollBar { }

                delegate: Rectangle {
                    id: row
                    width: ListView.view.width
                    height: 76
                    radius: Style.radiusCard
                    color: rowHover.hovered ? Style.surface2 : Style.surface
                    border.width: 1
                    border.color: modelData.duplicate ? Style.alpha(Style.danger, 0.4)
                                : rowHover.hovered ? Style.lineStrong : Style.line
                    Behavior on color { ColorAnimation { duration: Style.motionFast } }

                    HoverHandler { id: rowHover }

                    RowLayout {
                        anchors.fill: parent
                        anchors.margins: 12
                        spacing: 10

                        // 类型块
                        Rectangle {
                            Layout.preferredWidth: 38
                            Layout.preferredHeight: 38
                            Layout.alignment: Qt.AlignVCenter
                            radius: 10
                            color: modelData.inBox ? Style.accentSoft : Style.surface2

                            Text {
                                anchors.centerIn: parent
                                text: modelData.kind.length > 0
                                      ? modelData.kind.toUpperCase()
                                      : "FILE"
                                color: downloadsPage.stateColor(modelData)
                                font.pixelSize: modelData.kind.length > 2 ? 8 : 9
                                font.weight: Font.DemiBold
                                font.family: Style.fontFamily
                            }
                        }

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 3

                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 7

                                Text {
                                    Layout.fillWidth: true
                                    text: modelData.name
                                    color: Style.text
                                    font.pixelSize: Style.fontBody
                                    font.weight: Font.DemiBold
                                    font.family: Style.fontFamily
                                    elide: Text.ElideMiddle
                                }

                                Rectangle {
                                    height: 18
                                    width: stateText.implicitWidth + 12
                                    radius: 9
                                    color: modelData.failed ? Style.dangerSoft
                                         : modelData.inBox ? Style.accentSoft : Style.surface2

                                    Text {
                                        id: stateText
                                        anchors.centerIn: parent
                                        text: downloadsPage.stateLabel(modelData)
                                        color: downloadsPage.stateColor(modelData)
                                        font.pixelSize: 10
                                        font.family: Style.fontFamily
                                    }
                                }

                                Rectangle {
                                    visible: modelData.suggestDest.length > 0
                                    height: 18
                                    width: suggestText.implicitWidth + 12
                                    radius: 9
                                    color: Style.surface2

                                    Text {
                                        id: suggestText
                                        anchors.centerIn: parent
                                        text: "建议归档"
                                        color: Style.text2
                                        font.pixelSize: 10
                                        font.family: Style.fontFamily
                                    }
                                }

                                Rectangle {
                                    visible: modelData.duplicate
                                    height: 18
                                    width: dupText.implicitWidth + 12
                                    radius: 9
                                    color: Style.dangerSoft

                                    Text {
                                        id: dupText
                                        anchors.centerIn: parent
                                        text: "重复"
                                        color: Style.danger
                                        font.pixelSize: 10
                                        font.family: Style.fontFamily
                                    }
                                }
                            }

                            Text {
                                Layout.fillWidth: true
                                text: modelData.curPath
                                color: Style.text3
                                font.pixelSize: 10
                                font.family: Style.fontFamily
                                elide: Text.ElideMiddle
                            }

                            Text {
                                Layout.fillWidth: true
                                text: modelData.sizeText + " · "
                                      + (modelData.inBox ? "滞留 " + modelData.days + " 天"
                                                         : modelData.stateText)
                                color: Style.text3
                                font.pixelSize: 10
                                font.family: Style.fontFamily
                                elide: Text.ElideRight
                            }
                        }

                        RowLayout {
                            Layout.alignment: Qt.AlignVCenter
                            spacing: 2

                            AppIconButton {
                                icon: "external"
                                iconSize: 13
                                tooltip: "打开"
                                onClicked: inbox.openItem(modelData.id)
                            }
                            AppIconButton {
                                icon: "folder"
                                iconSize: 13
                                tooltip: "定位"
                                onClicked: inbox.revealItem(modelData.id)
                            }
                            AppIconButton {
                                icon: "more"
                                iconSize: 14
                                tooltip: "更多操作"
                                onClicked: rowMenu.popup()
                            }
                        }
                    }

                    Menu {
                        id: rowMenu

                        MenuItem {
                            text: "归档到建议位置"
                            enabled: modelData.inBox && modelData.suggestDest.length > 0
                            onTriggered: inbox.archiveItem(modelData.id)
                        }
                        MenuItem {
                            text: "加入待办"
                            onTriggered: inbox.addToTodo(modelData.id)
                        }
                        MenuItem {
                            text: "复制内容"
                            onTriggered: inbox.copyContent(modelData.id)
                        }
                        MenuItem {
                            text: "复制路径"
                            onTriggered: inbox.copyPath(modelData.id)
                        }
                        MenuItem {
                            text: "撤销接管"
                            enabled: modelData.inBox
                            onTriggered: inbox.undoItem(modelData.id)
                        }
                        MenuItem {
                            text: "撤销归档"
                            enabled: !modelData.inBox && modelData.trackState === "FiledAway"
                            onTriggered: inbox.unarchiveItem(modelData.id)
                        }
                        MenuSeparator { }
                        MenuItem {
                            text: "打开重复的原件"
                            visible: modelData.duplicate
                            onTriggered: inbox.revealDuplicate(modelData.id)
                        }
                        MenuItem {
                            text: "保留两份（清除重复标记）"
                            visible: modelData.duplicate
                            onTriggered: inbox.keepBoth(modelData.id)
                        }
                        MenuItem {
                            text: "删除重复副本（回收站）"
                            visible: modelData.duplicate
                            onTriggered: {
                                if (inbox.prepareDeleteDuplicate(modelData.id))
                                    deleteDupDialog.open()
                            }
                        }
                        MenuSeparator { }
                        MenuItem {
                            text: "重新定位（索引反查）"
                            enabled: !modelData.inBox
                            onTriggered: inbox.relocateItem(modelData.id)
                        }
                        MenuItem {
                            text: "标记为已删除"
                            enabled: !modelData.inBox && modelData.trackState !== "Deleted"
                            onTriggered: inbox.markDeleted(modelData.id)
                        }
                    }

                    Dialog {
                        id: deleteDupDialog
                        title: "删除重复副本"
                        modal: true
                        width: 380
                        anchors.centerIn: Overlay.overlay

                        contentItem: Text {
                            text: "两份文件内容完全一致。\n\n"
                                  + "将把「" + modelData.name + "」移入回收站；"
                                  + "原件不动，可从回收站还原。"
                            color: Style.text
                            font.pixelSize: Style.fontBody
                            font.family: Style.fontFamily
                            wrapMode: Text.WordWrap
                            width: deleteDupDialog.availableWidth
                        }

                        footer: DialogButtonBox {
                            Button {
                                text: "取消"
                                onClicked: deleteDupDialog.close()
                            }
                            Button {
                                text: "移入回收站"
                                onClicked: {
                                    inbox.deleteDuplicate(modelData.id)
                                    deleteDupDialog.close()
                                }
                            }
                        }
                    }
                }
            }

            AppEmptyState {
                anchors.centerIn: parent
                width: 360
                height: 180
                visible: list.count === 0
                icon: "download"
                title: downloadsPage.filterKey === "inbox" ? "收件箱是空的" : "还没有记录"
                hint: downloadsPage.filterKey === "inbox"
                      ? "开启「接管浏览器下载」后，新下载会先到这里，再决定归档或保留。"
                      : "切到「待整理」或「全部」看看；下载接管开启后才有新记录。"
            }
        }

        Text {
            Layout.fillWidth: true
            text: inbox.lastError
            color: Style.danger
            font.pixelSize: 10
            font.family: Style.fontFamily
            elide: Text.ElideRight
            visible: inbox.lastError.length > 0
        }
    }
}
