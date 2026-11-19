import QtQuick
import QtQuick.Layouts
import ".."

// 空状态：图标 + 标题 + 一句提示 + 可选动作。
ColumnLayout {
    id: root

    property string icon: "file"
    property string title: "这里还是空的"
    property string hint: ""
    property string actionText: ""

    signal actionClicked()

    spacing: 8
    Layout.alignment: Qt.AlignHCenter | Qt.AlignVCenter

    Rectangle {
        Layout.alignment: Qt.AlignHCenter
        width: 56
        height: 56
        radius: 18
        color: Style.surface2
        border.width: 1
        border.color: Style.line

        AppIcon {
            anchors.centerIn: parent
            name: root.icon
            size: 24
            iconColor: Style.text3
        }
    }

    Text {
        Layout.alignment: Qt.AlignHCenter
        text: root.title
        color: Style.text2
        font.pixelSize: Style.fontBody
        font.weight: Font.DemiBold
        font.family: Style.fontFamily
    }

    Text {
        visible: root.hint.length > 0
        Layout.alignment: Qt.AlignHCenter
        Layout.maximumWidth: 320
        text: root.hint
        color: Style.text3
        font.pixelSize: Style.fontCaption
        font.family: Style.fontFamily
        horizontalAlignment: Text.AlignHCenter
        wrapMode: Text.WordWrap
    }

    AppButton {
        visible: root.actionText.length > 0
        Layout.alignment: Qt.AlignHCenter
        Layout.topMargin: 4
        text: root.actionText
        variant: "soft"
        small: true
        onClicked: root.actionClicked()
    }
}
