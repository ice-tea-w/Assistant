import QtQuick
import QtQuick.Layouts

// 待办事项页（Phase 3 Step 3）
// vivo 原子笔记式「记录 → 完成划去」：勾选划去 + 沉底，底部输入框新增。
Rectangle {
    id: todoPage
    color: "transparent"

    // 主题色统一走 Style 单例（同目录 qmldir 的 singleton）
    readonly property bool isDark: Style.isDark
    readonly property color textColor:  Style.textColor
    readonly property color mutedColor: Style.mutedColor
    readonly property color accentColor: Style.accentColor
    readonly property color borderColor: Style.borderColor
    readonly property color panelColor: Style.panelColor

    ColumnLayout {
        anchors.fill: parent
        spacing: 10

        // 顶部标题
        Text {
            text: "待办事项"
            color: textColor
            font.pixelSize: 18
            font.bold: true
            Layout.leftMargin: 4
        }

        // 待办列表
        ListView {
            id: todoList
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: panel.todoList
            spacing: 4

            delegate: Rectangle {
                width: ListView.view.width
                height: 40
                radius: 6
                color: modelData.done ? "transparent" : panelColor

                // 行背景点击 → 勾选/取消
                MouseArea {
                    anchors.fill: parent
                    onClicked: panel.toggleTodo(modelData.id)
                }

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 10
                    anchors.rightMargin: 10
                    spacing: 10

                    // 勾选框
                    Rectangle {
                        Layout.preferredWidth: 18
                        Layout.preferredHeight: 18
                        radius: 4
                        color: modelData.done ? accentColor : "transparent"
                        border.color: modelData.done ? accentColor : borderColor
                        border.width: 1

                        Text {
                            anchors.centerIn: parent
                            text: "✓"
                            color: "white"
                            font.pixelSize: 12
                            font.bold: true
                            visible: modelData.done
                        }
                    }

                    // 待办内容（完成 → 删除线 + 置灰）
                    Text {
                        text: modelData.content
                        color: modelData.done ? mutedColor : textColor
                        font.pixelSize: 14
                        font.strikeout: modelData.done
                        Layout.fillWidth: true
                        elide: Text.ElideRight
                    }

                    // 删除按钮（始终可见，hover 高亮）
                    Rectangle {
                        Layout.preferredWidth: 24
                        Layout.preferredHeight: 24
                        radius: 4
                        color: delMouse.containsMouse ? borderColor : "transparent"

                        Text {
                            anchors.centerIn: parent
                            text: "✕"
                            color: delMouse.containsMouse ? textColor : mutedColor
                            font.pixelSize: 13
                        }

                        MouseArea {
                            id: delMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: panel.deleteTodo(modelData.id)
                        }
                    }
                }
            }

            Text {
                anchors.centerIn: parent
                text: "暂无待办，输入一条开始吧"
                color: mutedColor
                font.pixelSize: 13
                visible: todoList.count === 0
            }
        }

        // 底部新增输入框
        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 38
                radius: 6
                color: panelColor
                border.color: newInput.activeFocus ? accentColor : borderColor

                TextInput {
                    id: newInput
                    anchors.fill: parent
                    anchors.leftMargin: 10
                    anchors.rightMargin: 10
                    color: textColor
                    font.pixelSize: 14
                    verticalAlignment: TextInput.AlignVCenter

                    Text {
                        anchors.fill: parent
                        anchors.leftMargin: 10
                        visible: !newInput.text && !newInput.activeFocus
                        text: "添加待办…"
                        color: mutedColor
                        font.pixelSize: 14
                        verticalAlignment: Text.AlignVCenter
                    }

                    onAccepted: {
                        if (text.trim() !== "") {
                            panel.addTodo(text)
                            text = ""
                        }
                    }
                }
            }

            Rectangle {
                Layout.preferredWidth: 60
                Layout.preferredHeight: 38
                radius: 6
                color: accentColor

                Text {
                    anchors.centerIn: parent
                    text: "添加"
                    color: "white"
                    font.pixelSize: 14
                }

                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: {
                        if (newInput.text.trim() !== "") {
                            panel.addTodo(newInput.text)
                            newInput.text = ""
                        }
                    }
                }
            }
        }
    }
}
