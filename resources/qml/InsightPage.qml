import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "components"

// 数据看板（内置页，不依赖插件）
// 数据来自 MetricsBridge（context property `metrics`），全部只读聚合。
Rectangle {
    id: page
    color: "transparent"

    property int days: 30
    property var storageData: ({})
    property var downloadsData: ({})
    property var activityData: ({})
    property var kindsData: ({})

    readonly property var volumes: storageData.volumes || []
    readonly property var totals: downloadsData.totals || ({})
    readonly property var inbox: storageData.inbox || ({})
    readonly property var kindItems: kindsData.items || []

    function num(v) { return Number(v) || 0 }

    function fmtBytes(n) {
        n = num(n)
        if (n >= 1024 * 1024 * 1024) return (n / (1024 * 1024 * 1024)).toFixed(1) + " GB"
        if (n >= 1024 * 1024) return (n / (1024 * 1024)).toFixed(1) + " MB"
        if (n >= 1024) return (n / 1024).toFixed(0) + " KB"
        return n + " B"
    }

    function fmtCount(n) {
        n = num(n)
        if (n >= 10000) return (n / 10000).toFixed(1) + " 万"
        return String(n)
    }

    function reload() {
        storageData = metrics.storage()
        downloadsData = metrics.downloads(days)
        activityData = metrics.activity(days)
        kindsData = metrics.fileKinds()
    }

    Component.onCompleted: reload()

    Timer {
        interval: 60000
        running: true
        repeat: true
        onTriggered: page.reload()
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: Style.pagePadding
        spacing: 8

        // ---- 标题 + 时间范围 ----
        Item {
            id: headerItem
            Layout.fillWidth: false
            Layout.minimumWidth: page.width - 2 * Style.pagePadding
            Layout.maximumWidth: page.width - 2 * Style.pagePadding
            Layout.preferredWidth: page.width - 2 * Style.pagePadding
            Layout.preferredHeight: 32

            RowLayout {
                id: headerRow
                anchors.fill: parent
                spacing: 6

                ColumnLayout {
                    Layout.fillWidth: false
                    Layout.preferredWidth: headerItem.width - 238
                    spacing: 1
                    Text {
                        text: "数据看板"
                        color: Style.text
                        font.pixelSize: Style.fontTitle
                        font.weight: Font.DemiBold
                        font.family: Style.fontFamily
                    }
                    Text {
                        text: "本机 · 只读聚合 · 每 60 秒刷新"
                        color: Style.text3
                        font.pixelSize: 9
                        font.family: Style.fontFamily
                    }
                }

                Item {
                    id: rightItem
                    Layout.preferredWidth: 232
                    Layout.alignment: Qt.AlignVCenter

                    Row {
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 6

                        AppChip {
                            id: chip7
                            width: 52
                            text: "7 天"
                            selected: page.days === 7
                            onClicked: { page.days = 7; page.reload() }
                        }
                        AppChip {
                            width: 60
                            text: "30 天"
                            selected: page.days === 30
                            onClicked: { page.days = 30; page.reload() }
                        }
                        AppChip {
                            width: 60
                            text: "90 天"
                            selected: page.days === 90
                            onClicked: { page.days = 90; page.reload() }
                        }

                        AppIconButton {
                            icon: "refresh"
                            iconSize: 13
                            tooltip: "刷新"
                            onClicked: {
                                metrics.refresh()
                                page.reload()
                                toast.show("已刷新", "success")
                            }
                        }
                    }
                }
            }
        }

        // ---- KPI ----
        RowLayout {
            Layout.fillWidth: true
            Layout.preferredHeight: 50
            spacing: 8

            KpiCard {
                Layout.fillWidth: true
                label: "磁盘已用"
                value: num(storageData.percent).toFixed(0) + "%"
                sub: fmtBytes(storageData.used) + " / " + fmtBytes(storageData.total)
                accent: Style.accent
            }
            KpiCard {
                Layout.fillWidth: true
                label: "本期下载"
                value: fmtBytes(totals.bytes)
                sub: num(totals.count) + " 个 · 已归档 " + num(totals.archived)
                accent: Style.tileFile
            }
            KpiCard {
                Layout.fillWidth: true
                label: "待整理收件箱"
                value: num(inbox.pendingCount) + " 个"
                sub: fmtBytes(inbox.pendingBytes)
                accent: Style.warning
            }
        }

        // ---- 第一行：存储 / 下载 ----
        RowLayout {
            Layout.fillWidth: true
            Layout.preferredHeight: 140
            spacing: 8

            AppCard {
                Layout.fillWidth: true
                Layout.fillHeight: true
                padding: 8

                ColumnLayout {
                    width: parent.width
                    spacing: 4

                    RowLayout {
                        Layout.fillWidth: true
                        Text {
                            text: "存储容量"
                            color: Style.text2
                            font.pixelSize: 9
                            font.weight: Font.DemiBold
                            font.family: Style.fontFamily
                        }
                        Item { Layout.fillWidth: true }
                        Text {
                            text: volumes.length + " 个分区"
                            color: Style.text3
                            font.pixelSize: 8
                            font.family: Style.fontFamily
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 62
                        spacing: 6

                        DonutChart {
                            Layout.preferredWidth: 62
                            Layout.preferredHeight: 62
                            model: [
                                { "label": "已用", "value": num(storageData.used), "color": Style.accent },
                                { "label": "可用", "value": num(storageData.free), "color": Style.surface3 }
                            ]
                            centerText: num(storageData.percent).toFixed(0) + "%"
                        }

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 3
                            Text {
                                text: "已用 " + fmtBytes(storageData.used)
                                color: Style.text2
                                font.pixelSize: 8
                                font.family: Style.fontFamily
                            }
                            Text {
                                text: "可用 " + fmtBytes(storageData.free)
                                color: Style.text3
                                font.pixelSize: 8
                                font.family: Style.fontFamily
                            }
                            Text {
                                visible: volumes.length === 0
                                text: "未发现可用分区"
                                color: Style.text3
                                font.pixelSize: 8
                                font.family: Style.fontFamily
                            }
                        }
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 3

                        Repeater {
                            model: page.volumes
                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 5
                                Text {
                                    text: modelData.label
                                    color: Style.text2
                                    font.pixelSize: 8
                                    font.family: Style.fontFamily
                                    Layout.preferredWidth: 18
                                }
                                Rectangle {
                                    Layout.fillWidth: true
                                    height: 5
                                    radius: 3
                                    color: Style.surface3
                                    Rectangle {
                                        width: parent.width * Math.min(1, num(modelData.percent) / 100)
                                        height: parent.height
                                        radius: 3
                                        color: Style.accent
                                    }
                                }
                                Text {
                                    text: fmtBytes(modelData.used) + " / " + fmtBytes(modelData.total)
                                    color: Style.text3
                                    font.pixelSize: 7
                                    font.family: Style.fontFamily
                                    Layout.preferredWidth: 80
                                    horizontalAlignment: Text.AlignRight
                                }
                            }
                        }
                    }
                }
            }

            AppCard {
                Layout.fillWidth: true
                Layout.fillHeight: true
                padding: 8

                ColumnLayout {
                    width: parent.width
                    spacing: 3

                    RowLayout {
                        Layout.fillWidth: true
                        Text {
                            text: "下载量 · 近 " + page.days + " 天"
                            color: Style.text2
                            font.pixelSize: 9
                            font.weight: Font.DemiBold
                            font.family: Style.fontFamily
                        }
                        Item { Layout.fillWidth: true }
                        Text {
                            text: fmtBytes(totals.bytes) + " / " + num(totals.count) + " 个"
                            color: Style.text3
                            font.pixelSize: 8
                            font.family: Style.fontFamily
                        }
                    }

                    StackedBarChart {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        visible: num(totals.count) > 0
                        buckets: page.downloadsData.buckets || []
                        keys: [
                            { "key": "doc",   "label": "文档", "color": Style.tileFile },
                            { "key": "vid",   "label": "视频", "color": Style.tileCalc },
                            { "key": "zip",   "label": "压缩", "color": Style.tileDoc },
                            { "key": "other", "label": "其它", "color": Style.text3 }
                        ]
                    }

                    Item {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        Layout.preferredHeight: 80
                        visible: num(totals.count) === 0

                        Text {
                            anchors.centerIn: parent
                            width: parent.width - 12
                            text: "暂无已接管下载。开启「接管浏览器下载」后，新下载会在这里按天统计。"
                            color: Style.text3
                            font.pixelSize: 8
                            font.family: Style.fontFamily
                            wrapMode: Text.WordWrap
                            horizontalAlignment: Text.AlignHCenter
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8
                        visible: num(totals.count) > 0
                        Repeater {
                            model: [
                                { "label": "文档", "color": Style.tileFile },
                                { "label": "视频", "color": Style.tileCalc },
                                { "label": "压缩", "color": Style.tileDoc },
                                { "label": "其它", "color": Style.text3 }
                            ]
                            Row {
                                spacing: 3
                                Rectangle {
                                    width: 5; height: 5; radius: 2
                                    color: modelData.color
                                    anchors.verticalCenter: parent.verticalCenter
                                }
                                Text {
                                    text: modelData.label
                                    color: Style.text3
                                    font.pixelSize: 7
                                    font.family: Style.fontFamily
                                    anchors.verticalCenter: parent.verticalCenter
                                }
                            }
                        }
                        Item { Layout.fillWidth: true }
                    }
                }
            }
        }

        // ---- 第二行：活跃度 / 文件类型 ----
        RowLayout {
            Layout.fillWidth: true
            Layout.preferredHeight: 96
            spacing: 8

            AppCard {
                Layout.fillWidth: true
                Layout.fillHeight: true
                padding: 8

                ColumnLayout {
                    width: parent.width
                    spacing: 3

                    RowLayout {
                        Layout.fillWidth: true
                        Text {
                            text: "活跃度 · 14 周"
                            color: Style.text2
                            font.pixelSize: 9
                            font.weight: Font.DemiBold
                            font.family: Style.fontFamily
                        }
                        Item { Layout.fillWidth: true }
                        Text {
                            text: num((activityData.totals || {}).total) + " 次"
                            color: Style.text3
                            font.pixelSize: 8
                            font.family: Style.fontFamily
                        }
                    }

                    HeatmapChart {
                        Layout.alignment: Qt.AlignHCenter
                        weeks: 14
                        buckets: page.activityData.buckets || []
                        opacity: num((activityData.totals || {}).total) > 0 ? 1 : 0.35
                    }

                    Text {
                        visible: num((activityData.totals || {}).total) === 0
                        text: "还没有本地活动记录"
                        color: Style.text3
                        font.pixelSize: 8
                        font.family: Style.fontFamily
                    }
                }
            }

            AppCard {
                Layout.fillWidth: true
                Layout.fillHeight: true
                padding: 8

                ColumnLayout {
                    width: parent.width
                    spacing: 3

                    RowLayout {
                        Layout.fillWidth: true
                        Text {
                            text: "索引文件类型"
                            color: Style.text2
                            font.pixelSize: 9
                            font.weight: Font.DemiBold
                            font.family: Style.fontFamily
                        }
                        Item { Layout.fillWidth: true }
                        Text {
                            text: fmtCount(kindsData.entries) + " 个"
                            color: Style.text3
                            font.pixelSize: 8
                            font.family: Style.fontFamily
                        }
                    }

                    TopNBarChart {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        items: page.kindItems
                        maxRows: 4
                    }
                }
            }
        }

        // ---- 洞察条 ----
        RowLayout {
            Layout.fillWidth: true
            Layout.preferredHeight: 20
            spacing: 6

            Rectangle {
                width: 28
                height: 16
                radius: 8
                color: Style.accentSoft
                Text {
                    anchors.centerIn: parent
                    text: "洞察"
                    color: Style.accent
                    font.pixelSize: 7
                    font.family: Style.fontFamily
                }
            }

            Text {
                Layout.fillWidth: true
                text: "磁盘剩余 " + fmtBytes(storageData.free)
                      + " · 本期 " + num(totals.count) + " 个下载"
                      + " · 待整理 " + num(inbox.pendingCount) + " 个"
                      + " · 索引 " + fmtCount(kindsData.entries) + " 个文件"
                color: Style.text3
                font.pixelSize: 8
                font.family: Style.fontFamily
                elide: Text.ElideRight
            }
        }
    }

    AppToast { id: toast }
}
