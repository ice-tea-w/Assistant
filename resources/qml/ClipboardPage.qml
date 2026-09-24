import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// 剪贴板历史页（Phase 3 Step 5 + 收尾）
// 展示剪贴板文本历史，点击条目复制回剪贴板，支持一键清空。
// 时间显示改为相对时间（刚刚 / N 分钟前 / N 天前），悬停显示原始时间。
Rectangle {
    id: clipPage
    color: "transparent"

    readonly property bool isDark: Style.isDark
    readonly property color textColor:  Style.textColor
    readonly property color mutedColor: Style.mutedColor
    readonly property color accentColor: Style.accentColor
    readonly property color borderColor: Style.borderColor
    readonly property color panelColor: Style.panelColor

    // 相对时间的"刷新脉冲"：Date.now() 不是 QML 属性，绑不上依赖，
    // 所以每分钟自增一次，让下面引用它的绑定重新求值（时间才会自己走）。
    property int nowTick: 0

    Timer {
        interval: 60000
        running: true
        repeat: true
        onTriggered: clipPage.nowTick++
    }

    /// 把 epoch 秒转成"刚刚 / N 分钟前 / N 小时前 / N 天前"；
    /// 超过一周回退到日期（从 createdAt 里取 MM-DD）。
    function relativeTime(epoch, createdAt) {
        clipPage.nowTick   // 建立依赖，见上
        if (!epoch || epoch <= 0)
            return createdAt ? createdAt.substring(5, 10) : ""

        const diff = Math.floor(Date.now() / 1000) - epoch
        if (diff < 60)     return "刚刚"
        if (diff < 3600)   return Math.floor(diff / 60) + " 分钟前"
        if (diff < 86400)  return Math.floor(diff / 3600) + " 小时前"
        if (diff < 604800) return Math.floor(diff / 86400) + " 天前"
        return createdAt ? createdAt.substring(5, 10) : ""
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 10

        // 顶部工具栏
        RowLayout {
            Layout.fillWidth: true

            Text {
                text: "剪贴板历史"
                color: textColor
                font.pixelSize: Style.titleSize
                font.bold: true
                Layout.fillWidth: true
            }

            Rectangle {
                Layout.preferredWidth: 60
                Layout.preferredHeight: 30
                radius: Style.radiusSmall
                color: borderColor

                Text {
                    anchors.centerIn: parent
                    text: "清空"
                    color: textColor
                    font.pixelSize: Style.bodySize
                }

                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: panel.clearClips()
                }
            }
        }

        // 历史列表
        ListView {
            id: clipList
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: panel.clipList
            spacing: 4

            delegate: Rectangle {
                width: ListView.view.width
                height: 40
                radius: Style.radiusSmall
                color: clipMouse.containsMouse ? accentColor : panelColor

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 10
                    anchors.rightMargin: 10
                    spacing: 8

                    // 单行预览（把换行/连续空白压成单个空格）
                    Text {
                        text: modelData.content.replace(/\s+/g, " ")
                        color: clipMouse.containsMouse ? Style.accentTextColor : textColor
                        font.pixelSize: Style.bodySize
                        Layout.fillWidth: true
                        elide: Text.ElideRight
                    }

                    Text {
                        id: timeText
                        text: clipPage.relativeTime(modelData.createdAtEpoch, modelData.createdAt)
                        color: clipMouse.containsMouse ? Style.accentTextColor : mutedColor
                        font.pixelSize: 10
                        opacity: 0.8

                        // 悬停显示原始时间，需要精确值时不用猜
                        ToolTip.visible: clipMouse.containsMouse
                        ToolTip.delay: 600
                        ToolTip.text: modelData.createdAt
                    }
                }

                MouseArea {
                    id: clipMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: panel.copyClip(modelData.id)
                }
            }

            Text {
                anchors.centerIn: parent
                text: "暂无剪贴板记录\n复制任意文本后会自动记录"
                color: mutedColor
                font.pixelSize: Style.bodySize
                horizontalAlignment: Text.AlignHCenter
                visible: clipList.count === 0
            }
        }
    }
}
