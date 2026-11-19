import QtQuick
import QtQuick.Controls
import ".."

// 日历热力图：最近 weeks 周，每天一格；颜色按当天活动总量分级。
Item {
    id: root

    property var buckets: []
    property int weeks: 14
    readonly property int cell: 7
    readonly property int cellGap: 2
    readonly property var cells: {
        const n = weeks * 7
        const out = []
        const start = Math.max(0, buckets.length - n)
        let maxTotal = 0
        for (let i = start; i < buckets.length; ++i)
            maxTotal = Math.max(maxTotal, Number(buckets[i].total) || 0)

        for (let day = 0; day < 7; ++day) {
            for (let w = 0; w < weeks; ++w) {
                const idx = start + w * 7 + day
                const b = idx < buckets.length ? buckets[idx] : null
                const total = b ? (Number(b.total) || 0) : 0
                let level = 0
                if (maxTotal > 0 && total > 0) {
                    const r = total / maxTotal
                    level = r > 0.75 ? 4 : r > 0.5 ? 3 : r > 0.25 ? 2 : 1
                }
                out.push({
                    level: level,
                    date: b ? String(b.date) : "",
                    total: total
                })
            }
        }
        return out
    }

    implicitWidth: weeks * cell + (weeks - 1) * cellGap
    implicitHeight: 7 * cell + 6 * cellGap

    Grid {
        anchors.top: parent.top
        anchors.left: parent.left
        columns: root.weeks
        rows: 7
        flow: Grid.LeftToRight
        columnSpacing: root.cellGap
        rowSpacing: root.cellGap

        Repeater {
            model: root.cells

            Rectangle {
                width: root.cell
                height: root.cell
                radius: 2
                color: modelData.level === 0 ? Style.surface2
                     : Style.alpha(Style.accent, 0.18 + modelData.level * 0.18)

                HoverHandler { id: cellHover }
                ToolTip.visible: cellHover.hovered
                ToolTip.delay: 300
                ToolTip.text: modelData.date.length > 0
                              ? modelData.date + " · " + modelData.total + " 次活动"
                              : ""
            }
        }
    }
}
