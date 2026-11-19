import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "components"

// 待办事项（2026-11-19 UI 改版）
// 记录 → 勾选划去 → 沉底；交互与数据全部沿用 PanelBridge。
Rectangle {
    id: todoPage
    color: "transparent"

    readonly property int total: panel.todoList.length
    readonly property int doneCount: {
        var n = 0
        for (var i = 0; i < panel.todoList.length; ++i)
            if (panel.todoList[i].done) n++
        return n
    }

    function addTodo() {
        if (newInput.text.trim() === "") return
        panel.addTodo(newInput.text)
        newInput.text = ""
        toast.show("已添加待办", "success")
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: Style.pagePadding
        spacing: Style.spacingM

        AppSectionHeader {
            Layout.fillWidth: true
            Layout.preferredHeight: 48
            title: "待办事项"
            subtitle: total + " 项 · " + doneCount + " 项已完成"
        }

        // 分区标题
        RowLayout {
            Layout.fillWidth: true
            Layout.preferredHeight: 20
            spacing: 8

            Text {
                text: "全部待办"
                color: Style.text3
                font.pixelSize: 10
                font.weight: Font.DemiBold
                font.letterSpacing: 1.0
                font.family: Style.fontFamily
            }
            Text {
                text: total
                color: Style.text3
                font.pixelSize: 10
                font.family: Style.fontFamily
            }
            Rectangle {
                Layout.fillWidth: true
                height: 1
                color: Style.line
            }
        }

        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true

            ListView {
                id: todoList
                anchors.fill: parent
                clip: true
                model: panel.todoList
                spacing: 5
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: AppScrollBar { }

                delegate: Rectangle {
                    id: row
                    width: ListView.view.width
                    height: 50
                    radius: Style.radiusInput
                    color: modelData.done ? "transparent"
                         : rowHover.hovered ? Style.surface2 : Style.surface
                    border.width: modelData.done ? 0 : 1
                    border.color: Style.line
                    opacity: modelData.done ? 0.68 : 1.0
                    Behavior on color { ColorAnimation { duration: Style.motionFast } }

                    HoverHandler { id: rowHover }

                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: panel.toggleTodo(modelData.id)
                    }

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 12
                        anchors.rightMargin: 8
                        spacing: 10

                        AppCheckbox {
                            checked: modelData.done
                            onToggled: panel.toggleTodo(modelData.id)
                        }

                        Text {
                            Layout.fillWidth: true
                            text: modelData.content
                            color: modelData.done ? Style.text3 : Style.text
                            font.pixelSize: Style.fontBody
                            font.family: Style.fontFamily
                            font.strikeout: modelData.done
                            elide: Text.ElideRight
                        }

                        Text {
                            visible: !modelData.done
                            text: "待办"
                            color: Style.text3
                            font.pixelSize: Style.fontMicro
                            font.family: Style.fontFamily
                        }

                        AppIconButton {
                            visible: rowHover.hovered
                            icon: "trash"
                            iconSize: 13
                            danger: true
                            tooltip: "删除"
                            onClicked: {
                                panel.deleteTodo(modelData.id)
                                toast.show("已删除", "info")
                            }
                        }
                    }
                }
            }

            AppEmptyState {
                anchors.centerIn: parent
                width: 320
                height: 170
                visible: todoList.count === 0
                icon: "todo"
                title: "还没有待办"
                hint: "在下面输入一条，Enter 保存。完成后会自动划去并沉底。"
                actionText: "写第一条"
                onActionClicked: newInput.forceActiveFocus()
            }
        }

        // 新增
        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            AppTextField {
                id: newInput
                Layout.fillWidth: true
                placeholder: "添加待办，Enter 保存"
                leadingIcon: "plus"
                onAccepted: todoPage.addTodo()
            }

            AppButton {
                text: "添加"
                icon: "arrowUp"
                variant: "primary"
                onClicked: todoPage.addTodo()
            }
        }

        Text {
            Layout.fillWidth: true
            text: "本机保存 · 完成项自动置底 · 点击整行也可以勾选"
            color: Style.text3
            font.pixelSize: 10
            font.family: Style.fontFamily
            elide: Text.ElideRight
        }
    }

    AppToast { id: toast }
}
