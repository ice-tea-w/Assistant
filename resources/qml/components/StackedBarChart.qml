import QtQuick
import ".."

// 堆叠柱状图：buckets = [{ date, byKind: { key: bytes } }]
// keys = [{ key, label, color }]
Item {
    id: root

    property var buckets: []
    property var keys: []
    property int hoverIndex: -1
    readonly property int gap: 3
    readonly property real maxValue: {
        let m = 0
        for (let i = 0; i < buckets.length; ++i) {
            let sum = 0
            for (let k = 0; k < keys.length; ++k)
                sum += Number(buckets[i].byKind ? buckets[i].byKind[keys[k].key] : 0) || 0
            m = Math.max(m, sum)
        }
        return m > 0 ? m : 1
    }

    Item {
        id: plot
        anchors.fill: parent
        anchors.bottomMargin: 12

        Repeater {
            model: root.buckets

            Item {
                id: column
                property var bucket: modelData
                function bucketValue(key) {
                    return bucket && bucket.byKind ? (Number(bucket.byKind[key]) || 0) : 0
                }
                width: Math.max(2, (plot.width - root.gap * Math.max(0, root.buckets.length - 1))
                                     / Math.max(1, root.buckets.length))
                height: plot.height
                x: index * (width + root.gap)

                MouseArea {
                    anchors.fill: parent
                    hoverEnabled: true
                    onEntered: root.hoverIndex = index
                    onExited: if (root.hoverIndex === index) root.hoverIndex = -1
                }

                Column {
                    anchors.bottom: parent.bottom
                    width: parent.width
                    spacing: 0

                    Repeater {
                        model: root.keys

                        Rectangle {
                            width: column.width
                            height: {
                                const v = Number(column.bucketValue(modelData.key)) || 0
                                if (v <= 0) return 0
                                return Math.max(1, v / root.maxValue * plot.height)
                            }
                            color: modelData.color
                            radius: 1
                        }
                    }
                }
            }
        }

        Text {
            anchors.centerIn: parent
            visible: root.buckets.length === 0
            text: "暂无下载记录"
            color: Style.text3
            font.pixelSize: 9
            font.family: Style.fontFamily
        }
    }

    // x 轴只标首/中/尾，避免 30 根柱子把日期挤成一团
    Text {
        anchors.left: parent.left
        anchors.bottom: parent.bottom
        visible: root.buckets.length > 0
        text: root.buckets.length > 0 ? String(root.buckets[0].date).slice(5) : ""
        color: Style.text3
        font.pixelSize: 7
        font.family: Style.fontFamily
    }
    Text {
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        visible: root.buckets.length > 2
        text: root.buckets.length > 2
              ? String(root.buckets[Math.floor(root.buckets.length / 2)].date).slice(5) : ""
        color: Style.text3
        font.pixelSize: 7
        font.family: Style.fontFamily
    }
    Text {
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        visible: root.buckets.length > 0
        text: root.buckets.length > 0
              ? String(root.buckets[root.buckets.length - 1].date).slice(5) : ""
        color: Style.text3
        font.pixelSize: 7
        font.family: Style.fontFamily
    }

    Text {
        anchors.right: parent.right
        anchors.top: parent.top
        visible: root.hoverIndex >= 0 && root.hoverIndex < root.buckets.length
        text: root.hoverIndex >= 0 && root.hoverIndex < root.buckets.length
              ? String(root.buckets[root.hoverIndex].date).slice(5) : ""
        color: Style.text2
        font.pixelSize: 8
        font.family: Style.fontFamily
    }
}
