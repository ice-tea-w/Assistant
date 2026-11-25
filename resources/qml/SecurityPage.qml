import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "components"

// 安全中心（M3）：主程序侧只做展示与触发，能力由 tools/file_guard（Python）提供。
// 数据全部来自 SecurityBridge 的 `--json` 探针，异步、失败降级，不阻塞界面。
Rectangle {
    id: page
    color: "transparent"

    readonly property var cardData: (typeof security !== "undefined" && security.cards)
                                    ? security.cards : []
    readonly property bool busy: (typeof security !== "undefined") && security.busy

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
        if (state === "empty") return "未初始化"
        return "待检查"
    }

    function cardIcon(key) {
        if (key === "audit") return "history"
        if (key === "backup") return "folder"
        if (key === "vault") return "lock"
        if (key === "scan") return "eye"
        return "shield"
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
            subtitle: (typeof security !== "undefined")
                      ? security.summary
                      : "安全模块未连接（Missing SecurityBridge）"
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: Style.spacingM

            Repeater {
                model: page.cardData

                AppCard {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 116

                    ColumnLayout {
                        anchors.fill: parent
                        spacing: 6

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
                            Text {
                                text: page.stateText(modelData.state)
                                color: page.stateColor(modelData.state)
                                font.pixelSize: 11
                                font.weight: Font.DemiBold
                                font.family: Style.fontFamily
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
                    }
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: Style.spacingS

            AppButton {
                text: "运行安全体检"
                icon: "shield"
                variant: "primary"
                onClicked: {
                    if (typeof security !== "undefined") {
                        security.runSelfCheck()
                        page.notify("正在运行安全体检…")
                    }
                }
            }

            AppButton {
                text: "扫描下载目录"
                icon: "search"
                onClicked: {
                    if (typeof security !== "undefined")
                        security.runScan("")
                }
            }

            AppButton {
                text: "打开验收报告"
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

            Text {
                text: page.busy ? "正在检查…" : ""
                color: Style.text3
                font.pixelSize: 11
                font.family: Style.fontFamily
            }
        }

        AppCard {
            Layout.fillWidth: true
            Layout.fillHeight: true

            ColumnLayout {
                anchors.fill: parent
                spacing: 8

                Text {
                    text: "这个页面在整体项目里的位置"
                    color: Style.text
                    font.pixelSize: 13
                    font.weight: Font.DemiBold
                    font.family: Style.fontFamily
                }

                Text {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    color: Style.text2
                    font.pixelSize: 12
                    font.family: Style.fontFamily
                    text: "安全中心把 M3 安全模块（Python，tools/file_guard v0.6.0）接进主程序：" +
                          "审计链校验、备份库、保险箱、敏感扫描。所有调用都是异步子进程 + --json，" +
                          "不阻塞界面；Python 模块缺失时降级为只读状态，不影响主程序启动。"
                }

                Text {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    color: Style.text3
                    font.pixelSize: 12
                    font.family: Style.fontFamily
                    text: "当前边界：已提供可验证的安全能力（加密容器 / 审计链 / 备份恢复 / 保险箱）；" +
                          "M3 的 FileOpGateway 网关、策略判定与 security.db 权威库属于下一阶段，" +
                          "设计见 docs/architecture/四模块完整架构.md §6。"
                }

                Item { Layout.fillHeight: true }
            }
        }
    }
}
