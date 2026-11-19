import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "components"

// 剪贴板历史（2026-11-19 UI 改版）
// 顶部搜索 + 行内复制按钮 + 清空二次确认；相对时间保持原逻辑。
Rectangle {
    id: clipPage
    color: "transparent"

    property string query: ""
    property int nowTick: 0
    property bool confirmClear: false

    readonly property var rows: {
        var out = []
        var q = query.trim().toLowerCase()
        var list = panel.clipList
        for (var i = 0; i < list.length; ++i) {
            if (q === "" || String(list[i].content).toLowerCase().indexOf(q) >= 0)
                out.push(list[i])
        }
        return out
    }

    Timer {
        interval: 60000
        running: true
        repeat: true
        onTriggered: clipPage.nowTick++
    }

    Timer {
        id: confirmTimer
        interval: 3000
        onTriggered: clipPage.confirmClear = false
    }

    function relativeTime(epoch, createdAt) {
        clipPage.nowTick
        if (!epoch || epoch <= 0)
            return createdAt ? createdAt.substring(5, 10) : ""

        const diff = Math.floor(Date.now() / 1000) - epoch
        if (diff < 60)     return "刚刚"
        if (diff < 3600)   return Math.floor(diff / 60) + " 分钟前"
        if (diff < 86400)  return Math.floor(diff / 3600) + " 小时前"
        if (diff < 604800) return Math.floor(diff / 86400) + " 天前"
        return createdAt ? createdAt.substring(5, 10) : ""
    }

    function clearAll() {
        panel.clearClips()
        confirmClear = false
        toast.show("已清空剪贴板历史", "info")
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: Style.pagePadding
        spacing: Style.spacingM

        AppSectionHeader {
            Layout.fillWidth: true
            Layout.preferredHeight: 48
            title: "剪贴板"
            subtitle: panel.clipList.length + " 条记录 · 点击任意条目复制"

            AppButton {
                text: clipPage.confirmClear ? "确认清空" : "清空"
                icon: "trash"
                variant: clipPage.confirmClear ? "danger" : "ghost"
                small: true
                onClicked: {
                    if (clipPage.confirmClear) {
                        clipPage.clearAll()
                    } else {
                        clipPage.confirmClear = true
                        confirmTimer.restart()
                    }
                }
            }
        }

        AppTextField {
            Layout.fillWidth: true
            placeholder: "搜索剪贴板内容…"
            leadingIcon: "search"
            clearable: true
            onEdited: clipPage.query = text
        }

        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true

            ListView {
                id: clipList
                anchors.fill: parent
                clip: true
                model: clipPage.rows
                spacing: 4
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: AppScrollBar { }

                delegate: Rectangle {
                    id: clipRow
                    width: ListView.view.width
                    height: 48
                    radius: Style.radiusInput
                    color: clipRow.hovered ? Style.surface2 : Style.surface
                    border.width: 1
                    border.color: clipRow.hovered ? Style.lineStrong : Style.line
                    Behavior on color { ColorAnimation { duration: Style.motionFast } }

                    readonly property bool hovered: clipHover.hovered
                    HoverHandler { id: clipHover }

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 12
                        anchors.rightMargin: 8
                        spacing: 10

                        Rectangle {
                            Layout.preferredWidth: 28
                            Layout.preferredHeight: 28
                            radius: 8
                            color: Style.surface2
                            AppIcon {
                                anchors.centerIn: parent
                                name: "clip"
                                size: 14
                                iconColor: Style.text3
                            }
                        }

                        Text {
                            Layout.fillWidth: true
                            text: String(modelData.content).replace(/\s+/g, " ")
                            color: Style.text
                            font.pixelSize: Style.fontBody
                            font.family: Style.fontFamily
                            elide: Text.ElideRight
                        }

                        Text {
                            text: clipPage.relativeTime(modelData.createdAtEpoch, modelData.createdAt)
                            color: Style.text3
                            font.pixelSize: Style.fontMicro
                            font.family: Style.fontFamily

                            ToolTip.visible: clipHover.hovered
                            ToolTip.delay: 600
                            ToolTip.text: modelData.createdAt
                        }

                        AppIconButton {
                            visible: clipHover.hovered
                            icon: "copy"
                            iconSize: 13
                            tooltip: "复制"
                            onClicked: {
                                panel.copyClip(modelData.id)
                                toast.show("已复制到剪贴板", "success")
                            }
                        }
                    }

                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            panel.copyClip(modelData.id)
                            toast.show("已复制到剪贴板", "success")
                        }
                    }
                }
            }

            AppEmptyState {
                anchors.centerIn: parent
                width: 320
                height: 170
                visible: clipList.count === 0
                icon: "clip"
                title: clipPage.query.length > 0 ? "没有匹配的记录" : "剪贴板还是空的"
                hint: clipPage.query.length > 0
                      ? "换个关键词试试；清空搜索框可看全部记录"
                      : "复制任意文本后会自动记录在这里"
            }
        }
    }

    AppToast { id: toast }
}
