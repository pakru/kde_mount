import QtQuick
import QtQuick.Layouts
import QtQuick.Controls as QQC2

QQC2.Dialog {
    id: dialog
    modal: true
    title: "Connect"
    standardButtons: QQC2.Dialog.Cancel
    width: Math.min((parent ? parent.width : 640) - 40, 380)

    property string targetId: ""

    function openFor(id, username) {
        targetId = id
        userLabel.text = username.length > 0 ? ("as " + username) : "as guest"
        passwordField.text = ""
        open()
    }

    contentItem: ColumnLayout {
        spacing: 6
        QQC2.Label {
            id: userLabel
        }
        QQC2.Label {
            text: "Password:"
            visible: userLabel.text !== "as guest"
        }
        QQC2.TextField {
            id: passwordField
            Layout.fillWidth: true
            echoMode: TextInput.Password
            visible: userLabel.text !== "as guest"
        }
        QQC2.Button {
            Layout.alignment: Qt.AlignRight
            text: "Connect"
            onClicked: {
                kcm.actions.connectNow(targetId, passwordField.text)
                dialog.close()
            }
        }
    }
}
