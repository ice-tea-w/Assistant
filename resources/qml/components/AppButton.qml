import QtQuick
import QtQuick.Layouts
import ".."

// 统一按钮：primary / secondary / ghost / danger / soft
// 只发信号，不碰业务。
Item {
    id: root

    property string text: ""
    property string icon: ""
    property string variant: "secondary"
    property bool small: false
    property bool loading: false
    property int horizontalPadding: small ? 10 : 14
    property color customColor: "transparent"

    signal clicked()

    implicitWidth: content.implicitWidth + horizontalPadding * 2
    implicitHeight: small ? 28 : 34
    opacity: enabled ? 1.0 : 0.45
    activeFocusOnTab: true

    readonly property bool hovered: mouse.containsMouse
    readonly property bool pressed: mouse.pressed
    readonly property color fg: {
        if (variant === "danger") return Style.textOnDanger
        if (variant === "primary") return Style.textOnAccent
        if (variant === "soft") return Style.accent
        if (variant === "ghost") return Style.text2
        return Style.text
    }
    readonly property color fill: {
        if (variant === "primary")
            return pressed ? Style.accentPressed : (hovered ? Style.accentHover : Style.accent)
        if (variant === "danger")
            return pressed ? Qt.darker(Style.danger, 1.15) : (hovered ? Qt.lighter(Style.danger, 1.08) : Style.danger)
        if (variant === "soft")
            return hovered ? Style.alpha(Style.accent, 0.18) : Style.accentSoft
        if (variant === "ghost")
            return hovered ? Style.surface2 : "transparent"
        return hovered ? Style.surface3 : Style.surface2
    }

    Rectangle {
        id: bg
        anchors.fill: parent
        radius: Style.radiusControl
        color: root.fill
        border.width: root.variant === "secondary" ? 1 : 0
        border.color: Style.line
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

    RowLayout {
        id: content
        anchors.centerIn: parent
        spacing: 6

        AppIcon {
            visible: root.icon.length > 0
            name: root.icon
            size: root.small ? 14 : 16
            iconColor: root.fg
        }
        Text {
            visible: root.text.length > 0
            text: root.loading ? "处理中…" : root.text
            color: root.fg
            font.pixelSize: root.small ? Style.fontCaption : Style.fontBody
            font.weight: root.variant === "primary" || root.variant === "danger" ? Font.DemiBold : Font.Medium
            font.family: Style.fontFamily
        }
    }

    MouseArea {
        id: mouse
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        enabled: root.enabled && !root.loading
        onClicked: {
            root.forceActiveFocus()
            root.clicked()
        }
    }

    Keys.onSpacePressed: {
        if (enabled && !loading) clicked()
    }
    Keys.onReturnPressed: {
        if (enabled && !loading) clicked()
    }
}
