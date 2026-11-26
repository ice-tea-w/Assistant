import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "components"

// 安全模块里的账号管理：首次设置账号、登录、退出登录。
// 账号数据存放在 security 目录，口令用 PBKDF2 本地校验。
Dialog {
    id: dlg

    modal: true
    anchors.centerIn: parent
    width: 400
    title: "账号与登录"
    closePolicy: Popup.CloseOnEscape

    readonly property bool hasAuth: (typeof auth !== "undefined")

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
            text: dlg.hasAuth ? auth.statusText : "账号模块未就绪"
            color: (dlg.hasAuth && auth.signedIn) ? Style.success : Style.text2
            font.pixelSize: 12
            font.family: Style.fontFamily
            wrapMode: Text.WordWrap
        }

        Text {
            Layout.fillWidth: true
            text: "账号只保存在本机（安全中心数据目录），密码用 PBKDF2 存哈希；不联网也能登录。"
            color: Style.text3
            font.pixelSize: 11
            font.family: Style.fontFamily
            wrapMode: Text.WordWrap
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            AppTextField {
                id: userField
                Layout.fillWidth: true
                placeholder: "账号"
            }
            AppTextField {
                id: passField
                Layout.fillWidth: true
                placeholder: "密码（至少 6 位）"
                echoMode: TextInput.Password
                onAccepted: dlg.save()
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            AppButton {
                text: "保存账号"
                variant: "primary"
                onClicked: dlg.save()
            }
            AppButton {
                text: "登录"
                variant: "ghost"
                visible: dlg.hasAuth && auth.hasAccount && !auth.signedIn
                onClicked: if (dlg.hasAuth) auth.login(userField.text, passField.text)
            }
            AppButton {
                text: "退出登录"
                variant: "ghost"
                visible: dlg.hasAuth && auth.signedIn
                onClicked: if (dlg.hasAuth) auth.logout()
            }
            Item { Layout.fillWidth: true }
            AppButton {
                text: "关闭"
                variant: "ghost"
                onClicked: dlg.close()
            }
        }
    }

    function save() {
        if (!hasAuth) return
        auth.saveAccount(userField.text, passField.text)
    }

    onVisibleChanged: {
        if (visible) {
            passField.text = ""
            userField.text = (hasAuth && auth.userName !== "") ? auth.userName : ""
        }
    }
}
