import QtQuick
import ".."

// 统一输入框：可选前置图标、可清空、聚焦描边。
Rectangle {
    id: root

    property alias text: input.text
    property alias cursorPosition: input.cursorPosition
    property alias inputMethodComposing: input.inputMethodComposing
    property alias readOnly: input.readOnly
    property alias echoMode: input.echoMode
    property alias maximumLength: input.maximumLength
    property alias inputMethodHints: input.inputMethodHints

    property string placeholder: ""
    property string leadingIcon: ""
    property bool clearable: false
    property bool compact: false

    signal accepted()
    signal edited()
    signal userEdited()
    signal editingFinished()

    implicitHeight: compact ? 32 : 40
    radius: Style.radiusInput
    color: Style.surface
    border.width: input.activeFocus ? 1.5 : 1
    border.color: input.activeFocus ? Style.accent : Style.lineStrong
    Behavior on border.color { ColorAnimation { duration: Style.motionFast } }

    AppIcon {
        id: lead
        visible: root.leadingIcon.length > 0
        anchors.left: parent.left
        anchors.leftMargin: 11
        anchors.verticalCenter: parent.verticalCenter
        name: root.leadingIcon
        size: 15
        iconColor: input.activeFocus ? Style.accent : Style.text3
    }

    TextInput {
        id: input
        anchors.left: lead.visible ? lead.right : parent.left
        anchors.leftMargin: lead.visible ? 8 : 12
        anchors.right: clearBtn.visible ? clearBtn.left : parent.right
        anchors.rightMargin: clearBtn.visible ? 6 : 12
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        verticalAlignment: TextInput.AlignVCenter
        color: Style.text
        font.pixelSize: root.compact ? Style.fontBody : Style.fontBody
        font.family: Style.fontFamily
        selectByMouse: true
        activeFocusOnTab: true
        clip: true

        onAccepted: root.accepted()
        onTextChanged: root.edited()
        onTextEdited: root.userEdited()
        onEditingFinished: root.editingFinished()
    }

    Text {
        anchors.left: input.left
        anchors.right: input.right
        anchors.verticalCenter: parent.verticalCenter
        visible: !input.text && !input.activeFocus
        text: root.placeholder
        color: Style.text3
        font: input.font
        elide: Text.ElideRight
    }

    AppIconButton {
        id: clearBtn
        visible: root.clearable && input.text.length > 0
        anchors.right: parent.right
        anchors.rightMargin: 4
        anchors.verticalCenter: parent.verticalCenter
        icon: "close"
        iconSize: 11
        implicitWidth: 24
        implicitHeight: 24
        tooltip: "清空"
        onClicked: {
            input.text = ""
            input.forceActiveFocus()
        }
    }

    function forceActiveFocus() {
        input.forceActiveFocus()
    }

    function selectAll() {
        input.selectAll()
    }
}
