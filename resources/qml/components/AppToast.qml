import QtQuick
import ".."

// 轻提示：贴在父项底部居中，1.8 秒自动消失，不拦截鼠标。
Rectangle {
    id: root

    property string message: ""
    property string kind: "info"   // info | success | danger

    anchors.horizontalCenter: parent ? parent.horizontalCenter : undefined
    anchors.bottom: parent ? parent.bottom : undefined
    anchors.bottomMargin: 18
    z: 1000
    width: label.implicitWidth + 28
    height: 34
    radius: 9
    color: kind === "danger" ? Style.danger
         : kind === "success" ? Style.success
         : Style.surface3
    border.width: 1
    border.color: Style.lineStrong
    opacity: 0
    visible: opacity > 0
    enabled: false

    Text {
        id: label
        anchors.centerIn: parent
        text: root.message
        color: "#FFFFFF"
        font.pixelSize: Style.fontCaption
        font.family: Style.fontFamily
        font.weight: Font.Medium
    }

    Behavior on opacity { NumberAnimation { duration: Style.motionBase } }

    Timer {
        id: hideTimer
        interval: 1800
        onTriggered: root.opacity = 0
    }

    function show(msg, tone) {
        message = msg
        kind = tone !== undefined ? tone : "info"
        opacity = 1
        hideTimer.restart()
    }
}
