import QtQuick
import ".."

// 卡片容器：统一圆角、边框、内边距；内容默认挂到 contentItem。
Rectangle {
    id: root

    default property alias content: contentItem.data
    property int padding: Style.cardPadding
    property bool interactive: false
    property bool selected: false
    property bool danger: false

    readonly property bool hovered: hover.hovered && interactive

    implicitHeight: contentItem.childrenRect.height + padding * 2
    radius: Style.radiusCard
    color: danger ? Style.dangerSoft
         : selected ? Style.accentSoft
         : hovered ? Style.surface2
         : Style.surface
    border.width: 1
    border.color: danger ? Style.alpha(Style.danger, 0.45)
                : selected ? Style.accentBorder
                : Style.line
    Behavior on color { ColorAnimation { duration: Style.motionFast } }

    HoverHandler { id: hover }

    Item {
        id: contentItem
        anchors.fill: parent
        anchors.margins: root.padding
    }
}
