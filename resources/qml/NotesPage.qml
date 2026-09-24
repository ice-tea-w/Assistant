import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// 记事本页（Phase 3 Step 4）
// 左侧笔记列表 + 右侧编辑器；编辑防抖自动保存。
Rectangle {
    id: notesPage
    color: "transparent"

    // 主题色统一走 Style 单例（同目录 qmldir 的 singleton）
    readonly property bool isDark: Style.isDark
    readonly property color textColor:  Style.textColor
    readonly property color mutedColor: Style.mutedColor
    readonly property color accentColor: Style.accentColor
    readonly property color borderColor: Style.borderColor
    readonly property color panelColor: Style.panelColor

    // 当前编辑的笔记 id；-1 表示无
    property int currentNoteId: -1
    // 加载/新建时置 true，避免程序性赋值触发自动保存
    property bool loading: false

    function saveCurrent() {
        if (currentNoteId >= 0 && !loading) {
            panel.updateNote(currentNoteId, titleInput.text, contentEdit.text)
        }
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

    // 防抖自动保存
    Timer {
        id: saveTimer
        interval: 500
        onTriggered: notesPage.saveCurrent()
    }

    RowLayout {
        anchors.fill: parent
        spacing: 10

        // ---- 左侧笔记列表 ----
        Rectangle {
            Layout.preferredWidth: 200
            Layout.fillHeight: true
            radius: 6
            color: panelColor

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 8
                spacing: 6

                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 34
                    radius: 6
                    color: accentColor

                    Text {
                        anchors.centerIn: parent
                        text: "+ 新建笔记"
                        color: "white"
                        font.pixelSize: 13
                    }

                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: notesPage.newNote()
                    }
                }

                ListView {
                    id: notesList
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    model: panel.notesList
                    spacing: 4

                    delegate: Rectangle {
                        width: ListView.view.width
                        height: 44
                        radius: 6
                        color: modelData.id === notesPage.currentNoteId
                               ? accentColor : "transparent"

                        Column {
                            anchors.verticalCenter: parent.verticalCenter
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.leftMargin: 10
                            anchors.rightMargin: 10
                            spacing: 2

                            Text {
                                text: modelData.title !== "" ? modelData.title : "无标题"
                                color: modelData.id === notesPage.currentNoteId
                                       ? "white" : textColor
                                font.pixelSize: 14
                                font.bold: true
                                elide: Text.ElideRight
                                width: parent.width
                            }
                            Text {
                                text: modelData.updatedAt
                                color: modelData.id === notesPage.currentNoteId
                                       ? "white" : mutedColor
                                font.pixelSize: 10
                                opacity: 0.8
                            }
                        }

                        MouseArea {
                            anchors.fill: parent
                            onClicked: notesPage.loadNote(modelData.id)
                        }
                    }
                }
            }
        }

        // ---- 右侧编辑器 ----
        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 8

            // 标题
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 40
                radius: 6
                color: panelColor
                border.color: titleInput.activeFocus ? accentColor : borderColor

                TextInput {
                    id: titleInput
                    anchors.fill: parent
                    anchors.leftMargin: 12
                    anchors.rightMargin: 12
                    color: textColor
                    font.pixelSize: 16
                    font.bold: true
                    verticalAlignment: TextInput.AlignVCenter

                    Text {
                        anchors.fill: parent
                        anchors.leftMargin: 12
                        visible: !titleInput.text && !titleInput.activeFocus
                        text: "标题"
                        color: mutedColor
                        font.pixelSize: 16
                        verticalAlignment: Text.AlignVCenter
                    }

                    onTextChanged: {
                        if (!notesPage.loading) saveTimer.restart()
                    }
                }
            }

            // 正文
            Rectangle {
                Layout.fillWidth: true
                Layout.fillHeight: true
                radius: 6
                color: panelColor
                border.color: contentEdit.activeFocus ? accentColor : borderColor

                TextArea {
                    id: contentEdit
                    anchors.fill: parent
                    anchors.margins: 8
                    color: textColor
                    font.pixelSize: 14
                    placeholderText: "写点什么…"
                    placeholderTextColor: mutedColor
                    background: null

                    onTextChanged: {
                        if (!notesPage.loading) saveTimer.restart()
                    }
                }
            }

            // 底部工具栏
            RowLayout {
                Layout.fillWidth: true

                Text {
                    text: currentNoteId >= 0 ? "正在编辑" : "选择或新建笔记"
                    color: mutedColor
                    font.pixelSize: 12
                    Layout.fillWidth: true
                }

                Rectangle {
                    Layout.preferredWidth: 60
                    Layout.preferredHeight: 30
                    radius: 6
                    color: borderColor
                    visible: currentNoteId >= 0

                    Text {
                        anchors.centerIn: parent
                        text: "删除"
                        color: textColor
                        font.pixelSize: 13
                    }

                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            if (notesPage.currentNoteId >= 0) {
                                panel.deleteNote(notesPage.currentNoteId)
                                notesPage.currentNoteId = -1
                                titleInput.text = ""
                                contentEdit.text = ""
                            }
                        }
                    }
                }
            }
        }
    }
}
