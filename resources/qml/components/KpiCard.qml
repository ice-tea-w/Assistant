import QtQuick
import ".."

// 数据看板的 KPI 卡：标签 / 大数字 / 说明。
Rectangle {
    id: root

    property string label: ""
    property string value: ""
    property string sub: ""
    property color accent: Style.accent

    implicitHeight: 50
    radius: Style.radiusInput
    color: Style.surface
    border.width: 1
    border.color: Style.line

    Column {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        anchors.leftMargin: 10
        anchors.rightMargin: 10
        spacing: 1

        Row {
            spacing: 5
            Rectangle {
                width: 5
                height: 5
                radius: 2.5
                color: root.accent
            }
            Text {
                text: root.label
                color: Style.text3
                font.pixelSize: 9
                font.family: Style.fontFamily
            }
        }

        Text {
            text: root.value
            color: Style.text
            font.pixelSize: 15
            font.weight: Font.DemiBold
            font.family: Style.fontFamily
            elide: Text.ElideRight
            width: parent.width
        }

        Text {
            text: root.sub
            color: Style.text3
            font.pixelSize: 8
            font.family: Style.fontFamily
            elide: Text.ElideRight
            width: parent.width
        }
    }
}
