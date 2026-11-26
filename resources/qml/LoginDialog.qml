import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "components"

// 登录弹窗：未登录时弹出，可直接登录、注册账号或离线继续。
// 账号校验在本地完成（PBKDF2），不联网；离线继续则部分功能受限。
Dialog {
    id: dlg

    modal: true
    anchors.centerIn: parent
    width: 400
    title: dlg.registerMode ? "注册账号" : "登录 Assistant"
    closePolicy: Popup.NoAutoClose

    readonly property bool hasAuth: (typeof auth !== "undefined")
    readonly property bool hasAccount: hasAuth && auth.hasAccount
    readonly property bool registerMode2: registerMode
    property bool registerMode: false
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
            text: dlg.registerMode
                  ? (dlg.hasAccount
                        ? "用当前密码确认后，可注册并切换到新的本机账号。"
                        : "填写账号和密码，注册本机账号（离线可用）。")
                  : (dlg.hasAccount
                        ? "登录后可解除离线模式的功能限制；没有网络也可以登录（本地校验）。"
                        : "本机还没有账号：可以直接注册，或先离线继续。")
            color: Style.text2
            font.pixelSize: 12
            font.family: Style.fontFamily
            wrapMode: Text.WordWrap
        }

        // ---- 登录模式 ----
        ColumnLayout {
            Layout.fillWidth: true
            spacing: 8
            visible: dlg.hasAccount && !dlg.registerMode

            AppTextField {
                id: userField
                Layout.fillWidth: true
                placeholder: "账号"
            }
            AppTextField {
                id: passField
                Layout.fillWidth: true
                placeholder: "密码"
                echoMode: TextInput.Password
                onAccepted: dlg.tryLogin()
            }
        }

        // ---- 注册模式（无账号时也走这里）----
        ColumnLayout {
            Layout.fillWidth: true
            spacing: 8
            visible: dlg.registerMode || !dlg.hasAccount

            AppTextField {
                id: curPassField
                Layout.fillWidth: true
                visible: dlg.hasAccount && dlg.registerMode
                placeholder: "当前密码（确认身份）"
                echoMode: TextInput.Password
            }
            AppTextField {
                id: regUserField
                Layout.fillWidth: true
                placeholder: "账号"
            }
            AppTextField {
                id: regPassField
                Layout.fillWidth: true
                placeholder: "密码（至少 6 位）"
                echoMode: TextInput.Password
                onAccepted: dlg.tryRegister()
            }
        }

        Text {
            Layout.fillWidth: true
            visible: dlg.hasAuth && auth.loginLocked
            text: "登录已锁定：" + auth.lockRemainingSeconds + " 秒后重试"
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
                visible: dlg.hasAccount && !dlg.registerMode
                enabled: dlg.hasAuth && !auth.loginLocked
                         && userField.text.length > 0 && passField.text.length > 0
                onClicked: dlg.tryLogin()
            }

            AppButton {
                text: "注册账号"
                variant: "primary"
                visible: dlg.registerMode || !dlg.hasAccount
                enabled: dlg.hasAuth && !(dlg.hasAccount && dlg.registerMode && auth.loginLocked)
                onClicked: dlg.tryRegister()
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

            AppButton {
                text: dlg.registerMode ? "返回登录" : "注册新账号"
                variant: "ghost"
                visible: dlg.registerMode || dlg.hasAccount
                onClicked: {
                    dlg.registerMode = !dlg.registerMode
                    dlg.errorText = ""
                }
            }
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
        errorText = auth.signedIn ? "" : auth.statusText
        if (!auth.signedIn)
            passField.text = ""
    }

    function tryRegister() {
        if (!hasAuth) return
        if (hasAccount && registerMode) {
            auth.replaceAccount(regUserField.text, regPassField.text, curPassField.text)
        } else {
            auth.saveAccount(regUserField.text, regPassField.text)
        }
        errorText = auth.signedIn ? "" : auth.statusText
        if (!auth.signedIn)
            regPassField.text = ""
    }

    onVisibleChanged: {
        if (visible) {
            errorText = ""
            registerMode = !hasAccount
            passField.text = ""
            regPassField.text = ""
            curPassField.text = ""
            userField.text = (hasAuth && auth.hasAccount) ? auth.userName : ""
            regUserField.text = (hasAuth && auth.hasAccount) ? auth.userName : ""
        }
    }
}
