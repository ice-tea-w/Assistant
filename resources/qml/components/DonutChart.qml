import QtQuick
import ".."

// 环形图：纯 Canvas，不依赖 QtCharts。
// model: [{ label, value, color }]
Item {
    id: root

    property var model: []
    property int thickness: 9
    property string centerText: ""
    property string centerSub: ""
    readonly property bool dark: Style.isDark

    onModelChanged: canvas.requestPaint()
    onWidthChanged: canvas.requestPaint()
    onHeightChanged: canvas.requestPaint()
    onThicknessChanged: canvas.requestPaint()
    onDarkChanged: canvas.requestPaint()

    Canvas {
        id: canvas
        anchors.fill: parent
        antialiasing: true

        onPaint: {
            const ctx = getContext("2d")
            ctx.clearRect(0, 0, width, height)

            const cx = width / 2
            const cy = height / 2
            const radius = Math.max(8, Math.min(width, height) / 2 - root.thickness / 2 - 1)
            let total = 0
            for (let i = 0; i < root.model.length; ++i)
                total += Math.max(0, Number(root.model[i].value) || 0)

            ctx.lineWidth = root.thickness
            ctx.lineCap = "butt"

            if (total <= 0) {
                ctx.beginPath()
                ctx.arc(cx, cy, radius, 0, Math.PI * 2)
                ctx.strokeStyle = Style.surface3
                ctx.stroke()
                return
            }

            let start = -Math.PI / 2
            for (let i = 0; i < root.model.length; ++i) {
                const v = Math.max(0, Number(root.model[i].value) || 0)
                if (v <= 0)
                    continue
                const sweep = Math.PI * 2 * v / total
                const gap = sweep > 0.06 ? 0.018 : 0
                ctx.beginPath()
                ctx.arc(cx, cy, radius, start + gap, start + sweep - gap)
                ctx.strokeStyle = String(root.model[i].color)
                ctx.stroke()
                start += sweep
            }
        }
    }

    Column {
        anchors.centerIn: parent
        spacing: 0
        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            text: root.centerText
            color: Style.text
            font.pixelSize: 14
            font.weight: Font.DemiBold
            font.family: Style.fontFamily
        }
        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            visible: text.length > 0
            text: root.centerSub
            color: Style.text3
            font.pixelSize: 8
            font.family: Style.fontFamily
        }
    }
}
