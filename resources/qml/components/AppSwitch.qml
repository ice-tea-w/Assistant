import QtQuick
import ".."

// 设置开关：36×20。只发 toggled，不自己改 checked（让配置属性回写，保持绑定）。
Item {
    id: root

    property bool checked: false

    signal toggled(bool checked)

    implicitWidth: 38
    implicitHeight: 22

    Rectangle {
        id: track
        anchors.centerIn: parent
        width: 38
        height: 22
        radius: 11
        color: root.checked ? Style.accent : Style.surface3
        border.width: root.checked ? 0 : 1
        border.color: Style.lineStrong
        Behavior on color { ColorAnimation { duration: Style.motionBase } }
    }

    Rectangle {
        id: knob
        width: 18
        height: 18
        radius: 9
        y: 2
        x: root.checked ? 18 : 2
        color: "#FFFFFF"
        border.width: 1
        border.color: Qt.rgba(0, 0, 0, 0.08)
        Behavior on x { NumberAnimation { duration: Style.motionBase; easing.type: Easing.OutCubic } }
    }

    MouseArea {
        anchors.fill: parent
        cursorShape: Qt.PointingHandCursor
        onClicked: root.toggled(!root.checked)
    }
}
