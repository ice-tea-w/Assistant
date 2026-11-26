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
        if (state === "warn") return Style.warning
        return Style.text3
    }

    function cardHint(key) {
        if (key === "audit")
            return "审计链：重要操作会写成一条带 HMAC 的链式记录，改一条就会断链。\n这里显示当前记录数和最新链头；点击打开审计目录。"
        if (key === "backup")
            return "备份库：文件版本备份的加密索引。\n显示已有版本数和总大小；点击打开备份目录。"
        if (key === "vault")
            return "保险箱：用口令加密文件的容器。\n这里只显示是否已初始化（解锁需要口令）；点击打开保险箱目录。"
        if (key === "scan")
            return "敏感扫描：检查身份证、银行卡、手机号、统一社会信用代码等敏感信息，结果已打码。\n点击查看扫描报告（没有报告时会提示先扫描）。"
        return ""
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
                    property string cardKey: modelData.key
                    Layout.fillWidth: true
                    Layout.minimumWidth: 110
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
                            Layout.minimumWidth: 0
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

                    MouseArea {
                        id: cardArea
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            if (typeof security !== "undefined")
                                security.openCard(modelData.key)
                        }
                        ToolTip {
                            id: cardTip
                            visible: cardArea.containsMouse
                            delay: 350
                            timeout: 15000
                            contentItem: Text {
                                width: 300
                                text: page.cardHint(cardKey)
                                color: Style.text
                                font.pixelSize: 12
                                font.family: Style.fontFamily
                                wrapMode: Text.WordWrap
                            }
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
                text: "重置审计链"
                icon: "history"
                variant: "ghost"
                enabled: !page.healthRunning
                onClicked: resetDialog.open()
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

                ListView {
                    id: stepList
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    spacing: 2
                    model: page.steps
                    boundsBehavior: Flickable.StopAtBounds
                    onCountChanged: positionViewAtEnd()

                    delegate: RowLayout {
                        width: stepList.width
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

                    ScrollBar.vertical: ScrollBar {}
                }
            }
        }
    }

    Dialog {
        id: resetDialog
        title: "重置审计链"
        modal: true
        anchors.centerIn: parent
        width: 380

        contentItem: Text {
            text: "历史审计记录将被清空，且不可恢复。\n重置后会写入一条「手动重置」记录。确定继续吗？"
            color: Style.text
            font.pixelSize: 12
            font.family: Style.fontFamily
            wrapMode: Text.WordWrap
        }

        standardButtons: Dialog.Cancel | Dialog.Ok

        onAccepted: {
            if (typeof security !== "undefined")
                security.resetAudit()
        }
    }



}
