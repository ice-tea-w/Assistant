import QtQuick
import ".."

// 过滤 Chip：选中只铺 accentSoft，不做整块实心蓝。
Rectangle {
    id: root

    property string text: ""
    property bool selected: false
    property int count: -1

    signal clicked()

    implicitWidth: label.implicitWidth + (root.count >= 0 ? 26 : 22)
    implicitHeight: 26
    radius: 13
    color: selected ? Style.accentSoft : (mouse.containsMouse ? Style.surface2 : "transparent")
    border.width: selected ? 1 : 0
    border.color: Style.accentBorder
    Behavior on color { ColorAnimation { duration: Style.motionFast } }

    Row {
        anchors.centerIn: parent
        spacing: 4

        Text {
            id: label
            text: root.text
            color: root.selected ? Style.accent : Style.text2
            font.pixelSize: Style.fontCaption
            font.weight: root.selected ? Font.DemiBold : Font.Normal
            font.family: Style.fontFamily
        }
        Text {
            visible: root.count >= 0
            text: root.count
            color: Style.text3
            font.pixelSize: Style.fontMicro
            font.family: Style.fontFamily
        }
    }

    MouseArea {
        id: mouse
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onClicked: root.clicked()
    }
}
