import QtQuick
import QtQuick.Layouts
import ".."

// 页面标题：标题 + 副标题 + 右侧动作。
Item {
    id: root

    property string title: ""
    property string subtitle: ""
    default property alias actions: actionRow.data

    implicitHeight: Math.max(titleColumn.implicitHeight, actionRow.implicitHeight)

    RowLayout {
        anchors.fill: parent
        spacing: 12

        ColumnLayout {
            id: titleColumn
            Layout.fillWidth: true
            spacing: 3

            Text {
                text: root.title
                color: Style.text
                font.pixelSize: Style.fontTitle
                font.weight: Font.DemiBold
                font.family: Style.fontFamily
                elide: Text.ElideRight
                Layout.fillWidth: true
            }
            Text {
                visible: root.subtitle.length > 0
                text: root.subtitle
                color: Style.text3
                font.pixelSize: Style.fontCaption
                font.family: Style.fontFamily
                elide: Text.ElideRight
                Layout.fillWidth: true
            }
        }

        RowLayout {
            id: actionRow
            spacing: 6
            Layout.alignment: Qt.AlignVCenter
        }
    }
}
