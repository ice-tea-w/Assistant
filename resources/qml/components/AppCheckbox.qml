import QtQuick
import ".."

// 待办勾选框：20×20，勾选时填充 accent 并带一次缩放动画。
Item {
    id: root

    property bool checked: false

    signal toggled(bool checked)

    implicitWidth: 20
    implicitHeight: 20

    Rectangle {
        id: box
        anchors.centerIn: parent
        width: 20
        height: 20
        radius: 6
        color: root.checked ? Style.accent : "transparent"
        border.width: root.checked ? 0 : 1.5
        border.color: Style.lineStrong
        Behavior on color { ColorAnimation { duration: Style.motionFast } }

        AppIcon {
            anchors.centerIn: parent
            name: "check"
            size: 12
            weight: 2.8
            iconColor: Style.textOnAccent
            opacity: root.checked ? 1 : 0
            scale: root.checked ? 1 : 0.6
            Behavior on opacity { NumberAnimation { duration: Style.motionFast } }
            Behavior on scale { NumberAnimation { duration: Style.motionBase; easing.type: Easing.OutCubic } }
        }
    }

    MouseArea {
        anchors.fill: parent
        anchors.margins: -4
        cursorShape: Qt.PointingHandCursor
        onClicked: root.toggled(!root.checked)
    }
}
