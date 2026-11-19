import QtQuick
import QtQuick.Controls
import ".."

// 图标按钮：hover 才出现底色；用于行内操作、标题栏、导航。
Item {
    id: root

    property string icon: "more"
    property string tooltip: ""
    property int iconSize: 14
    property bool danger: false
    property bool checked: false

    signal clicked()

    implicitWidth: 28
    implicitHeight: 28
    activeFocusOnTab: true

    readonly property bool hovered: mouse.containsMouse
    readonly property color fg: {
        if (danger && hovered) return Style.textOnDanger
        if (checked) return Style.accent
        return hovered ? Style.text : Style.text3
    }

    Rectangle {
        anchors.fill: parent
        radius: Style.radiusControl
        color: {
            if (danger && hovered) return Style.danger
            if (checked) return Style.accentSoft
            if (hovered) return Style.surface2
            return "transparent"
        }
        Behavior on color { ColorAnimation { duration: Style.motionFast } }
    }

    Rectangle {
        anchors.fill: parent
        anchors.margins: -2
        radius: Style.radiusControl + 2
        color: "transparent"
        border.width: 2
        border.color: Style.accent
        opacity: root.activeFocus ? 0.55 : 0
        visible: opacity > 0
        z: 3
    }

    AppIcon {
        anchors.centerIn: parent
        name: root.icon
        size: root.iconSize
        iconColor: root.fg
    }

    MouseArea {
        id: mouse
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        enabled: root.enabled
        onClicked: {
            root.forceActiveFocus()
            root.clicked()
        }
    }

    ToolTip.visible: hovered && tooltip.length > 0
    ToolTip.delay: 500
    ToolTip.text: tooltip

    Keys.onSpacePressed: clicked()
    Keys.onReturnPressed: clicked()
}
