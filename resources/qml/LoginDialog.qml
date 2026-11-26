import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "components"

// 登录弹窗：启动 / 进入主面板时，未登录且未选择离线则弹出。
// 账号校验在本地完成（PBKDF2），不联网；也可以选择离线继续（部分功能受限）。
Dialog {
    id: dlg

    modal: true
    anchors.centerIn: parent
    width: 380
    title: "登录 Assistant"
    closePolicy: Popup.NoAutoClose

    readonly property bool hasAuth: (typeof auth !== "undefined")
    readonly property bool hasAccount: hasAuth && auth.hasAccount
    property string errorText: ""

    visible: hasAuth && auth.needsLogin

    background: Rectangle {
        color: Style.surface
        radius: Style.radiusCard
        border.width: 1
        border.color: Style.lineStrong
    }

    contentItem: ColumnLayout {
        width: dlg.availableWidth
        spacing: 10

        Text {
            Layout.fillWidth: true
            text: dlg.hasAccount
                  ? "登录后可解除离线模式的功能限制；没有网络也可以登录（本地校验）。"
                  : "本机还没有账号：请先在 设置 → 账号 注册账号，或先离线继续。"
            color: Style.text2
            font.pixelSize: 12
            font.family: Style.fontFamily
            wrapMode: Text.WordWrap
        }

        AppTextField {
            id: userField
            Layout.fillWidth: true
            visible: dlg.hasAccount
            placeholder: "账号"
        }

        AppTextField {
            id: passField
            Layout.fillWidth: true
            visible: dlg.hasAccount
            placeholder: "密码"
            echoMode: TextInput.Password
            onAccepted: dlg.tryLogin()
        }

        Text {
            Layout.fillWidth: true
            visible: dlg.hasAuth && auth.loginLocked
            text: (dlg.hasAuth && auth.loginLocked)
                  ? "登录已锁定：" + auth.lockRemainingSeconds + " 秒后重试"
                  : ""
            color: Style.warning
            font.pixelSize: 11
            font.family: Style.fontFamily
            wrapMode: Text.WordWrap
        }

        Text {
            Layout.fillWidth: true
            visible: dlg.errorText !== ""
            text: dlg.errorText
            color: Style.danger
            font.pixelSize: 11
            font.family: Style.fontFamily
            wrapMode: Text.WordWrap
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            AppButton {
                text: "登录"
                variant: "primary"
                visible: dlg.hasAccount
                enabled: dlg.hasAuth && !auth.loginLocked
                         && userField.text.length > 0 && passField.text.length > 0
                onClicked: dlg.tryLogin()
            }

            AppButton {
                text: "离线继续（受限）"
                variant: "ghost"
                onClicked: {
                    if (dlg.hasAuth)
                        auth.continueOffline()
                }
            }

            Item { Layout.fillWidth: true }
        }

        Text {
            Layout.fillWidth: true
            text: "离线模式下：设备同步、插件市场不可用；账号密码可在 设置 → 账号 修改。"
            color: Style.text3
            font.pixelSize: 10
            font.family: Style.fontFamily
            wrapMode: Text.WordWrap
        }
    }

    function tryLogin() {
        if (!hasAuth) return
        auth.login(userField.text, passField.text)
        if (!auth.signedIn) {
            errorText = auth.statusText
            passField.text = ""
        } else {
            errorText = ""
        }
    }

    onVisibleChanged: {
        if (visible) {
            errorText = ""
            passField.text = ""
            userField.text = (hasAuth && auth.hasAccount) ? auth.userName : ""
        }
    }
}
