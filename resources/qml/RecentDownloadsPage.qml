import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Phase C：「最近下载」—— 收件箱（待整理）与已移出记录。
//
// 数据来自 InboxBridge（downloads.db）。本阶段没有去向追踪，所以"已移出"只表示
// 文件已不在收件箱，位置未知；Phase D 接上 TrackingEngine 后这里会显示真实路径。
Rectangle {
    id: downloadsPage
    color: "transparent"

    readonly property bool isDark:      Style.isDark
    readonly property color textColor:  Style.textColor
    readonly property color mutedColor: Style.mutedColor
    readonly property color accentColor: Style.accentColor
    readonly property color borderColor: Style.borderColor
    readonly property color panelColor: Style.panelColor
    readonly property color dangerColor: Style.dangerColor

    property string filterKey: "inbox"
    property var rows: []

    function reload() {
        rows = inbox.filterItems(filterKey)
    }

    function stateColor(item) {
        if (item.failed) return dangerColor
        if (item.inBox) return accentColor
        return mutedColor
    }

    Connections {
        target: inbox
        function onItemsChanged() { downloadsPage.reload() }
    }

    Component.onCompleted: reload()

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 8
        spacing: 8

        // ---- 标题 + 统计 ----
        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            Text {
                text: "最近下载"
                color: textColor
                font.pixelSize: Style.titleSize
                font.bold: true
            }

            Text {
                text: inbox.pendingCount + " 个待整理"
                color: mutedColor
                font.pixelSize: 12
            }

            Item { Layout.fillWidth: true }

            Button {
                text: "刷新"
                font.pixelSize: 12
                onClicked: inbox.refresh()
            }
        }

        // ---- 筛选 Chip ----
        RowLayout {
            spacing: 6

            Repeater {
                model: [
                    { "key": "inbox", "label": "待整理" },
                    { "key": "filed", "label": "已移出" },
                    { "key": "all",   "label": "全部" }
                ]

                delegate: Rectangle {
                    height: 24
                    radius: 12
                    width: chipText.implicitWidth + 20
                    color: downloadsPage.filterKey === modelData.key ? accentColor : panelColor
                    border.color: borderColor

                    Text {
                        id: chipText
                        anchors.centerIn: parent
                        text: modelData.label
                        font.pixelSize: 11
                        color: downloadsPage.filterKey === modelData.key
                               ? Style.accentTextColor : textColor
                    }

                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            downloadsPage.filterKey = modelData.key
                            downloadsPage.reload()
                        }
                    }
                }
            }
        }

        // ---- 列表 ----
        ListView {
            id: list
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            spacing: 4
            model: downloadsPage.rows

            delegate: Rectangle {
                width: list.width
                height: 70
                radius: 6
                color: panelColor
                border.color: borderColor

                RowLayout {
                    anchors.fill: parent
                    anchors.margins: 8
                    spacing: 10

                    Rectangle {
                        Layout.preferredWidth: 34
                        Layout.preferredHeight: 34
                        radius: 6
                        color: "transparent"
                        border.width: 1
                        border.color: downloadsPage.stateColor(modelData)

                        Text {
                            anchors.centerIn: parent
                            text: modelData.kind.length > 0 ? modelData.kind.toUpperCase() : "FILE"
                            color: downloadsPage.stateColor(modelData)
                            font.pixelSize: 9
                            font.bold: true
                        }
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 2

                        RowLayout {
                            spacing: 8
                            Text {
                                text: modelData.name
                                color: textColor
                                font.pixelSize: 13
                                font.bold: true
                                elide: Text.ElideRight
                                Layout.fillWidth: true
                            }
                            Text {
                                text: modelData.stateText
                                color: downloadsPage.stateColor(modelData)
                                font.pixelSize: 11
                            }
                            Text {
                                visible: modelData.suggestDest.length > 0
                                text: "建议归档"
                                color: accentColor
                                font.pixelSize: 10
                            }
                        }

                        Text {
                            text: modelData.sizeText + " · " +
                                  (modelData.inBox ? "滞留 " + modelData.days + " 天"
                                                   : modelData.stateText)
                            color: mutedColor
                            font.pixelSize: 11
                        }

                        Text {
                            text: modelData.curPath
                            color: mutedColor
                            font.pixelSize: 11
                            elide: Text.ElideMiddle
                            Layout.fillWidth: true
                        }
                    }

                    ColumnLayout {
                        spacing: 4

                        RowLayout {
                            spacing: 4
                            Button {
                                text: "打开"
                                font.pixelSize: 11
                                onClicked: inbox.openItem(modelData.id)
                            }
                            Button {
                                text: "定位"
                                font.pixelSize: 11
                                onClicked: inbox.revealItem(modelData.id)
                            }
                        }

                        Button {
                            visible: modelData.inBox
                            text: "撤销接管"
                            font.pixelSize: 11
                            onClicked: inbox.undoItem(modelData.id)
                        }

                        Button {
                            text: "⋯"
                            font.pixelSize: 11
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
                }
            }

            Text {
                anchors.centerIn: parent
                text: downloadsPage.filterKey === "inbox"
                      ? "收件箱是空的 —— 开启「接管浏览器下载」后，新下载会先到这里"
                      : "还没有记录"
                color: mutedColor
                font.pixelSize: 12
                width: parent.width - 20
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
                visible: list.count === 0
            }
        }

        // ---- 错误提示（撤销失败等）----
        Text {
            Layout.fillWidth: true
            text: inbox.lastError
            color: dangerColor
            font.pixelSize: 11
            elide: Text.ElideRight
            visible: inbox.lastError.length > 0
        }
    }
}
