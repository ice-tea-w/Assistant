import QtQuick
import QtQuick.Controls
import ".."

// 统一下拉框：替换 Fusion 默认的白底/灰边。
ComboBox {
    id: control

    implicitHeight: 32
    implicitWidth: 180
    font.pixelSize: Style.fontBody
    font.family: Style.fontFamily
    leftPadding: 10
    rightPadding: 28

    background: Rectangle {
        radius: Style.radiusControl
        color: control.hovered ? Style.surface2 : Style.surface
        border.width: 1
        border.color: control.activeFocus ? Style.accent : Style.lineStrong
        Behavior on color { ColorAnimation { duration: Style.motionFast } }
    }

    contentItem: Text {
        leftPadding: 10
        rightPadding: 28
        text: control.displayText
        color: control.enabled ? Style.text : Style.text3
        font: control.font
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }

    indicator: AppIcon {
        x: control.width - width - 10
        y: (control.height - height) / 2
        name: "chevronDown"
        size: 13
        iconColor: Style.text3
    }

    delegate: ItemDelegate {
        id: delegateItem
        width: control.width
        height: 30
        highlighted: control.highlightedIndex === index

        contentItem: Text {
            text: control.textRole ? model[control.textRole] : modelData
            color: Style.text
            font.pixelSize: Style.fontBody
            font.family: Style.fontFamily
            verticalAlignment: Text.AlignVCenter
            leftPadding: 10
        }

        background: Rectangle {
            radius: 6
            color: delegateItem.highlighted ? Style.surface2 : "transparent"
        }
    }
}
