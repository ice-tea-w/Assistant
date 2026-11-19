import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "components"

// 记事本（2026-11-19 UI 改版）
// 左列表 + 右编辑器；500ms 防抖自动保存；选中项用 accentSoft，不再整块实心蓝。
Rectangle {
    id: notesPage
    color: "transparent"

    property int currentNoteId: -1
    property bool loading: false

    function saveCurrent() {
        if (currentNoteId >= 0 && !loading)
            panel.updateNote(currentNoteId, titleInput.text, contentEdit.text)
    }

    function loadNote(id) {
        saveCurrent()
        currentNoteId = id
        loading = true
        const list = panel.notesList
        for (let i = 0; i < list.length; i++) {
            if (list[i].id === id) {
                titleInput.text = list[i].title
                contentEdit.text = list[i].content
                break
            }
        }
        loading = false
    }

    function newNote() {
        saveCurrent()
        const id = panel.addNote("", "")
        if (id >= 0) {
            currentNoteId = id
            loading = true
            titleInput.text = ""
            contentEdit.text = ""
            loading = false
            titleInput.forceActiveFocus()
        }
    }

    function deleteCurrent() {
        if (currentNoteId < 0) return
        panel.deleteNote(currentNoteId)
        currentNoteId = -1
        loading = true
        titleInput.text = ""
        contentEdit.text = ""
        loading = false
        toast.show("笔记已删除", "info")
    }

    Timer {
        id: saveTimer
        interval: 500
        onTriggered: notesPage.saveCurrent()
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: Style.pagePadding
        spacing: Style.spacingM

        AppSectionHeader {
            Layout.fillWidth: true
            Layout.preferredHeight: 48
            title: "记事本"
            subtitle: panel.notesList.length + " 篇笔记 · 自动保存"

            AppButton {
                text: "新建"
                icon: "plus"
                small: true
                variant: "primary"
                onClicked: notesPage.newNote()
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 10

            // ---- 左侧列表 ----
            Rectangle {
                Layout.preferredWidth: 240
                Layout.fillHeight: true
                radius: Style.radiusCard
                color: Style.surface
                border.width: 1
                border.color: Style.line

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 10
                    spacing: 8

                    RowLayout {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 18
                        spacing: 6

                        Text {
                            text: "全部笔记"
                            color: Style.text3
                            font.pixelSize: 10
                            font.weight: Font.DemiBold
                            font.letterSpacing: 1.0
                            font.family: Style.fontFamily
                        }
                        Text {
                            text: panel.notesList.length
                            color: Style.text3
                            font.pixelSize: 10
                            font.family: Style.fontFamily
                        }
                        Item { Layout.fillWidth: true }
                    }

                    ListView {
                        id: notesList
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        Layout.preferredHeight: 1
                        Layout.minimumHeight: 0
                        clip: true
                        model: panel.notesList
                        spacing: 4
                        boundsBehavior: Flickable.StopAtBounds
                        ScrollBar.vertical: AppScrollBar { }

                        delegate: Rectangle {
                            id: noteRow
                            width: ListView.view.width
                            height: 52
                            radius: Style.radiusControl
                            color: modelData.id === notesPage.currentNoteId
                                   ? Style.accentSoft
                                   : noteHover.hovered ? Style.surface2 : "transparent"
                            Behavior on color { ColorAnimation { duration: Style.motionFast } }

                            HoverHandler { id: noteHover }

                            ColumnLayout {
                                anchors.fill: parent
                                anchors.leftMargin: 10
                                anchors.rightMargin: 10
                                spacing: 2

                                Text {
                                    Layout.fillWidth: true
                                    text: modelData.title !== "" ? modelData.title : "无标题"
                                    color: modelData.id === notesPage.currentNoteId
                                           ? Style.accent : Style.text
                                    font.pixelSize: 13
                                    font.weight: Font.DemiBold
                                    font.family: Style.fontFamily
                                    elide: Text.ElideRight
                                }
                                Text {
                                    Layout.fillWidth: true
                                    text: modelData.updatedAt
                                    color: Style.text3
                                    font.pixelSize: 10
                                    font.family: Style.fontFamily
                                    elide: Text.ElideRight
                                }
                            }

                            MouseArea {
                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor
                                onClicked: notesPage.loadNote(modelData.id)
                            }
                        }
                    }

                    AppEmptyState {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 160
                        visible: panel.notesList.length === 0
                        icon: "note"
                        title: "还没有笔记"
                        hint: "点右上角「新建」开始"
                    }
                }
            }

            // ---- 右侧编辑器 ----
            Item {
                Layout.fillWidth: true
                Layout.fillHeight: true

                ColumnLayout {
                    anchors.fill: parent
                    visible: notesPage.currentNoteId >= 0
                    spacing: 8

                    AppTextField {
                        id: titleInput
                        Layout.fillWidth: true
                        placeholder: "标题"
                        onEdited: {
                            if (!notesPage.loading) saveTimer.restart()
                        }
                    }

                    AppCard {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        padding: 10

                        TextArea {
                            id: contentEdit
                            anchors.fill: parent
                            color: Style.text
                            font.pixelSize: Style.fontBody
                            font.family: Style.fontFamily
                            placeholderText: "写点什么…"
                            placeholderTextColor: Style.text3
                            wrapMode: TextArea.Wrap
                            selectByMouse: true
                            background: null
                            topPadding: 4
                            leftPadding: 4

                            onTextChanged: {
                                if (!notesPage.loading) saveTimer.restart()
                            }
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 30
                        spacing: 8

                        Text {
                            Layout.fillWidth: true
                            text: "自动保存 · 关闭页面也会保留"
                            color: Style.text3
                            font.pixelSize: 10
                            font.family: Style.fontFamily
                        }

                        AppButton {
                            text: "删除"
                            icon: "trash"
                            variant: "ghost"
                            small: true
                            onClicked: notesPage.deleteCurrent()
                        }
                    }
                }

                AppEmptyState {
                    anchors.centerIn: parent
                    width: 340
                    height: 180
                    visible: notesPage.currentNoteId < 0
                    icon: "note"
                    title: "选择或新建一篇笔记"
                    hint: "左侧列表点一下即可编辑；所有修改会在 500ms 后自动保存。"
                    actionText: "新建笔记"
                    onActionClicked: notesPage.newNote()
                }
            }
        }
    }

    AppToast { id: toast }
}
