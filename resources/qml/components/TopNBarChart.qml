import QtQuick
import ".."

// 横条 Top N：items = [{ label, count, bytes }]
Item {
    id: root

    property var items: []
    property int maxRows: 5

    function fmt(n) {
        n = Number(n) || 0
        if (n >= 1024 * 1024 * 1024) return (n / (1024 * 1024 * 1024)).toFixed(1) + " GB"
        if (n >= 1024 * 1024) return (n / (1024 * 1024)).toFixed(1) + " MB"
        if (n >= 1024) return (n / 1024).toFixed(0) + " KB"
        return n + " B"
    }

    readonly property real maxValue: {
        let m = 1
        for (let i = 0; i < Math.min(items.length, maxRows); ++i)
            m = Math.max(m, Number(items[i].bytes) || 0)
        return m
    }

    Column {
        anchors.fill: parent
        spacing: 2

        Repeater {
            model: Math.min(root.items.length, root.maxRows)

            Item {
                width: parent.width
                height: 14

                Text {
                    anchors.left: parent.left
                    anchors.verticalCenter: parent.verticalCenter
                    width: 42
                    text: root.items[index].label
                    color: Style.text3
                    font.pixelSize: 8
                    font.family: Style.fontFamily
                    elide: Text.ElideRight
                }

                Rectangle {
                    anchors.left: parent.left
                    anchors.leftMargin: 46
                    anchors.right: valueText.left
                    anchors.rightMargin: 6
                    anchors.verticalCenter: parent.verticalCenter
                    height: 6
                    radius: 3
                    color: Style.surface3

                    Rectangle {
                        width: parent.width * Math.min(1, (Number(root.items[index].bytes) || 0)
                                                       / root.maxValue)
                        height: parent.height
                        radius: 3
                        color: Style.accent
                    }
                }

                Text {
                    id: valueText
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    width: 44
                    horizontalAlignment: Text.AlignRight
                    text: root.fmt(root.items[index].bytes)
                    color: Style.text3
                    font.pixelSize: 8
                    font.family: Style.fontFamily
                }
            }
        }

        Text {
            visible: root.items.length === 0
            text: "索引尚未就绪"
            color: Style.text3
            font.pixelSize: 9
            font.family: Style.fontFamily
        }
    }
}
