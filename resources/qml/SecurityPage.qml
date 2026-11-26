import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "components"

// 安全中心（M3）：C++ 安全模块直接编译进主程序。
// 上半部分是四张状态卡，下半部分是「运行体检」的逐步过程。
Rectangle {
    id: page
    color: "transparent"

    readonly property var cardData: (typeof security !== "undefined" && security.cards)
                                    ? security.cards : []
    readonly property var steps: (typeof security !== "undefined" && security.healthSteps)
                                 ? security.healthSteps : []
    readonly property bool healthRunning: (typeof security !== "undefined") && security.healthRunning

    function stateColor(state) {
        if (state === "ok") return Style.success
        if (state === "error") return Style.danger
        if (state === "warn") return Style.warning
        return Style.text3
    }

    function stateText(state) {
        if (state === "ok") return "正常"
        if (state === "error") return "异常"
        if (state === "warn") return "有发现"
        return ""
    }

    function cardIcon(key) {
        if (key === "audit") return "history"
        if (key === "backup") return "folder"
        if (key === "vault") return "lock"
        if (key === "scan") return "eye"
        return "shield"
    }

    function stepColor(state) {
        if (state === "ok") return Style.success
        if (state === "error") return Style.danger
        if (state === "running") return Style.warning
        return Style.text3
    }

    function notify(text) {
        if (typeof toast !== "undefined")
            toast.show(text, "success")
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: Style.pagePadding
        spacing: Style.spacingM

        AppSectionHeader {
            Layout.fillWidth: true
            Layout.preferredHeight: 48
            title: "安全中心"
            subtitle: (typeof security !== "undefined") ? security.summary : "安全模块未就绪"
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: Style.spacingM

            Repeater {
                model: page.cardData

                AppCard {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 130

                    ColumnLayout {
                        anchors.fill: parent
                        spacing: 8

                        RowLayout {
                            spacing: 8
                            AppIcon {
                                name: page.cardIcon(modelData.key)
                                size: 16
                                iconColor: Style.text2
                            }
                            Text {
                                Layout.fillWidth: true
                                text: modelData.title
                                color: Style.text
                                font.pixelSize: 13
                                font.weight: Font.DemiBold
                                font.family: Style.fontFamily
                            }
                            Rectangle {
                                width: 7
                                height: 7
                                radius: 4
                                color: page.stateColor(modelData.state)
                            }
                        }

                        Text {
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            text: modelData.detail
                            color: Style.text2
                            font.pixelSize: 12
                            wrapMode: Text.WordWrap
                            elide: Text.ElideRight
                            maximumLineCount: 3
                            font.family: Style.fontFamily
                        }

                        Text {
                            text: page.stateText(modelData.state)
                            color: page.stateColor(modelData.state)
                            font.pixelSize: 11
                            font.weight: Font.DemiBold
                            font.family: Style.fontFamily
                        }
                    }
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: Style.spacingS

            AppButton {
                text: page.healthRunning ? "体检中…" : "运行安全体检"
                icon: "shield"
                variant: "primary"
                enabled: !page.healthRunning
                onClicked: {
                    if (typeof security !== "undefined") {
                        security.runSelfCheck()
                        page.notify("开始安全体检…")
                    }
                }
            }

            AppButton {
                text: "扫描下载目录"
                icon: "search"
                enabled: !(typeof security !== "undefined" && security.busy)
                onClicked: {
                    if (typeof security !== "undefined")
                        security.runScan("")
                }
            }

            AppButton {
                text: "打开安全报告"
                icon: "external"
                variant: "ghost"
                onClicked: {
                    if (typeof security !== "undefined")
                        security.openReport()
                }
            }

            AppButton {
                text: "打开数据目录"
                icon: "folder"
                variant: "ghost"
                onClicked: {
                    if (typeof security !== "undefined")
                        security.openDataDir()
                }
            }

            Item { Layout.fillWidth: true }
        }

        AppCard {
            Layout.fillWidth: true
            Layout.fillHeight: true

            ColumnLayout {
                anchors.fill: parent
                spacing: 6

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8

                    Text {
                        text: "体检过程"
                        color: Style.text
                        font.pixelSize: 13
                        font.weight: Font.DemiBold
                        font.family: Style.fontFamily
                    }
                    Item { Layout.fillWidth: true }
                    Text {
                        text: page.healthRunning
                              ? "检查中…"
                              : (page.steps.length > 0 ? "已完成" : "")
                        color: page.healthRunning ? Style.warning : Style.text3
                        font.pixelSize: 11
                        font.family: Style.fontFamily
                    }
                }

                Text {
                    visible: page.steps.length === 0
                    Layout.fillWidth: true
                    text: "点击「运行安全体检」，这里会一步步显示检查过程。"
                    color: Style.text3
                    font.pixelSize: 12
                    wrapMode: Text.WordWrap
                    font.family: Style.fontFamily
                }

                Repeater {
                    model: page.steps

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8

                        Rectangle {
                            width: 6
                            height: 6
                            radius: 3
                            color: page.stepColor(modelData.state)
                        }
                        Text {
                            Layout.preferredWidth: 72
                            text: modelData.title
                            color: Style.text
                            font.pixelSize: 12
                            font.family: Style.fontFamily
                        }
                        Text {
                            Layout.fillWidth: true
                            text: modelData.detail
                            color: Style.text2
                            font.pixelSize: 12
                            elide: Text.ElideRight
                            font.family: Style.fontFamily
                        }
                        Text {
                            text: modelData.time
                            color: Style.text3
                            font.pixelSize: 11
                            font.family: Style.fontFamily
                        }
                    }
                }

                Item { Layout.fillHeight: true }
            }
        }
    }
}
