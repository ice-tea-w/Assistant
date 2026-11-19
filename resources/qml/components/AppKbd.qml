import QtQuick
import ".."

// 键帽：底部快捷键提示用。
Rectangle {
    id: root

    property string text: ""

    implicitWidth: Math.max(18, label.implicitWidth + 8)
    implicitHeight: 19
    radius: 5
    color: Style.surface2
    border.width: 1
    border.color: Style.lineStrong

    Text {
        id: label
        anchors.centerIn: parent
        text: root.text
        color: Style.text3
        font.pixelSize: 9
        font.family: Style.fontFamily
        font.weight: Font.Medium
    }
}
