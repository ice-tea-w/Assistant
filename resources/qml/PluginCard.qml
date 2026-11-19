import QtQuick
import QtQuick.Layouts
import ".."
import "components"

// 插件卡片（2026-11-19 换皮）
//
// 约束保持不变：
//   1. 固定 64px 高，状态变化不得让列表跳动；
//   2. 只有信号没有动作；
//   3. 视觉由 state 决定。
Rectangle {
    id: card

    property string pluginId: ""
    property string name: ""
    property string version: ""
    property string summary: ""
    property string icon: ""
    property string state: "not-installed"
    property string badge: ""
    property string badgeKind: ""
    property string problem: ""
    property int    percent: 0
    property bool   pluginEnabled: true
    property bool   demo: false

    signal installClicked()
    signal cancelClicked()
    signal toggleToggled(bool want)
    signal uninstallClicked()
    signal detailClicked()
    signal updateClicked()

    readonly property bool isProblem: state === "problem"
    readonly property bool isInstalling: state === "installing"
    readonly property bool isInstalled: state === "installed"
                                  || state === "update-available"
                                  || state === "bundled"
    readonly property bool isBundled: state === "bundled"
    readonly property bool isIncompatible: state === "incompatible"
    readonly property bool isUpdate: state === "update-available"
    readonly property bool dimmed: isInstalled && !pluginEnabled

    height: 64
    radius: Style.radiusCard
    color: isProblem ? Style.dangerSoft
         : cardHover.hovered ? Style.surface2 : Style.surface
    border.width: 1
    border.color: isProblem ? Style.alpha(Style.danger, 0.45)
                : cardHover.hovered ? Style.lineStrong : Style.line
    opacity: dimmed ? 0.62 : 1.0
    Behavior on opacity { NumberAnimation { duration: Style.motionFast } }
    Behavior on color { ColorAnimation { duration: Style.motionFast } }

    HoverHandler { id: cardHover }

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 12
        anchors.rightMargin: 12
        spacing: 10

        // 图标：插件自带 emoji 时保留，否则用统一的 plugin 图标
        Rectangle {
            Layout.preferredWidth: 34
            Layout.preferredHeight: 34
            Layout.alignment: Qt.AlignVCenter
            radius: 9
            color: isProblem ? Style.alpha(Style.danger, 0.12) : Style.surface2
            border.width: 1
            border.color: Style.line

            Text {
                anchors.centerIn: parent
                visible: card.icon.length > 0
                text: card.icon
                font.pixelSize: 18
            }
            AppIcon {
                anchors.centerIn: parent
                visible: card.icon.length === 0
                name: card.isProblem ? "alert" : "plugin"
                size: 17
                iconColor: card.isProblem ? Style.danger : Style.text3
            }
        }

        ColumnLayout {
            Layout.fillWidth: true
            spacing: 3

            RowLayout {
                Layout.fillWidth: true
                spacing: 6

                Text {
                    text: card.name
                    color: Style.text
                    font.pixelSize: 13
                    font.weight: Font.DemiBold
                    font.family: Style.fontFamily
                    elide: Text.ElideRight
                    Layout.maximumWidth: card.width * 0.42
                }

                Text {
                    text: "v" + card.version
                    color: Style.text3
                    font.pixelSize: 10
                    font.family: Style.fontFamily
                }

                Rectangle {
                    visible: card.badge !== ""
                    height: 17
                    width: badgeText.implicitWidth + 12
                    radius: 8.5
                    color: {
                        switch (card.badgeKind) {
                        case "accent": return Style.accentSoft
                        case "red":    return Style.dangerSoft
                        case "orange": return Style.alpha(Style.warning, 0.14)
                        default:       return Style.surface2
                        }
                    }

                    Text {
                        id: badgeText
                        anchors.centerIn: parent
                        text: card.badge
                        font.pixelSize: 9
                        font.family: Style.fontFamily
                        color: {
                            switch (card.badgeKind) {
                            case "accent": return Style.accent
                            case "red":    return Style.danger
                            case "orange": return Style.warning
                            default:       return Style.text3
                            }
                        }
                    }
                }

                Item { Layout.fillWidth: true }
            }

            Text {
                Layout.fillWidth: true
                text: card.isProblem && card.problem !== "" ? card.problem : card.summary
                color: card.isProblem ? Style.danger : Style.text3
                font.pixelSize: 10
                font.family: Style.fontFamily
                elide: Text.ElideRight
                maximumLineCount: 1
            }
        }

        // 右侧动作
        Item {
            Layout.preferredWidth: 132
            Layout.preferredHeight: 32
            Layout.alignment: Qt.AlignVCenter

            // 未安装 / 不兼容 / 示例
            AppButton {
                visible: card.state === "not-installed" || card.isIncompatible
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                text: card.isIncompatible ? "不兼容" : (card.demo ? "未接入" : "安装")
                icon: card.isIncompatible || card.demo ? "" : "download"
                variant: card.isIncompatible || card.demo ? "secondary" : "primary"
                small: true
                enabled: !card.isIncompatible && !card.demo
                onClicked: card.installClicked()
            }

            // 安装中
            RowLayout {
                visible: card.isInstalling
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                spacing: 6

                ColumnLayout {
                    spacing: 3
                    Layout.preferredWidth: 64

                    Rectangle {
                        Layout.fillWidth: true
                        height: 3
                        radius: 2
                        color: Style.surface3

                        Rectangle {
                            width: parent.width * Math.max(0, Math.min(1, card.percent / 100))
                            height: parent.height
                            radius: 2
                            color: Style.accent
                            Behavior on width { NumberAnimation { duration: 150 } }
                        }
                    }
                    Text {
                        Layout.alignment: Qt.AlignRight
                        text: card.percent + "%"
                        font.pixelSize: 9
                        font.family: Style.fontFamily
                        color: Style.accent
                    }
                }

                AppIconButton {
                    icon: "close"
                    iconSize: 11
                    tooltip: "取消安装"
                    onClicked: card.cancelClicked()
                }
            }

            // 有问题
            AppButton {
                visible: card.isProblem
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                text: "卸载"
                icon: "trash"
                variant: "danger"
                small: true
                onClicked: card.uninstallClicked()
            }

            // 已安装
            RowLayout {
                visible: card.isInstalled && !card.isProblem
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                spacing: 6

                AppButton {
                    visible: card.isUpdate
                    text: "更新"
                    variant: "primary"
                    small: true
                    onClicked: card.updateClicked()
                }

                AppSwitch {
                    checked: card.pluginEnabled
                    onToggled: card.toggleToggled(checked)
                }

                AppIconButton {
                    icon: "more"
                    iconSize: 14
                    tooltip: "详情"
                    onClicked: card.detailClicked()
                }
            }
        }
    }
}
