/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

import QtQuick
import QtQuick.Layouts
import QtQuick.Controls as QQC2
import QtQuick.Dialogs as QtDialogs

QQC2.Dialog {
    id: dialog
    modal: true
    title: "Add network mount"
    standardButtons: QQC2.Dialog.Cancel
    width: Math.min((parent ? parent.width : 640) - 40, 520)

    function openForAdd() {
        uncField.text = "//"
        pathField.text = ""
        userField.text = ""
        domainField.text = ""
        passwordField.text = ""
        rememberCheck.checked = true
        reconnectCheck.checked = true
        systemModeRadio.checked = false
        sessionModeRadio.checked = true
        open()
    }

    QtDialogs.FolderDialog {
        id: folderDialog
        onAccepted: pathField.text = selectedFolder.toString().replace("file://", "")
    }

    contentItem: ColumnLayout {
        spacing: 6

        QQC2.Label { text: "Share (//host/share[/subdir]):" }
        QQC2.TextField {
            id: uncField
            Layout.fillWidth: true
        }

        QQC2.Label { text: "Mount point:" }
        RowLayout {
            Layout.fillWidth: true
            QQC2.TextField {
                id: pathField
                Layout.fillWidth: true
                placeholderText: "/home/you/ShareName"
            }
            QQC2.Button {
                text: "Browse…"
                onClicked: folderDialog.open()
            }
        }

        QQC2.Label { text: "Username (leave empty for guest access):" }
        QQC2.TextField {
            id: userField
            Layout.fillWidth: true
            // Guest selection clears the fields it disables below (design
            // §7.4.4), not just visually hides them -- otherwise stale text
            // left in a disabled field is silently sent as guest-inconsistent
            // input and the save is confusingly rejected.
            onTextChanged: {
                if (text.length === 0) {
                    passwordField.text = ""
                    domainField.text = ""
                    rememberCheck.checked = false
                }
            }
        }

        QQC2.Label { text: "Password:" }
        QQC2.TextField {
            id: passwordField
            Layout.fillWidth: true
            echoMode: TextInput.Password
            enabled: userField.text.length > 0
        }

        QQC2.Label { text: "Domain (optional):" }
        QQC2.TextField {
            id: domainField
            Layout.fillWidth: true
            enabled: userField.text.length > 0
        }

        QQC2.CheckBox {
            id: rememberCheck
            text: "Remember password in KWallet"
            visible: sessionModeRadio.checked
            enabled: userField.text.length > 0
        }

        QQC2.Label {
            text: "When should this share be mounted?"
            font.bold: true
            Layout.topMargin: 6
        }
        QQC2.ButtonGroup { id: modeGroup }
        QQC2.RadioButton {
            id: sessionModeRadio
            text: "Only while I'm signed in (recommended)"
            checked: true
            QQC2.ButtonGroup.group: modeGroup
        }
        QQC2.RadioButton {
            id: systemModeRadio
            text: "Available at startup, before anyone signs in"
            QQC2.ButtonGroup.group: modeGroup
        }

        QQC2.Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            visible: systemModeRadio.checked && userField.text.length > 0
            text: "Stores this share's password in a file on this computer, readable by administrators. "
                + "Same as a hand-written system mount."
        }
        QQC2.Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            visible: systemModeRadio.checked && userField.text.length === 0
            text: "This share does not require a password; its mount definition will still be activated at "
                + "startup."
        }

        QQC2.CheckBox {
            id: reconnectCheck
            text: "Arm automatically at sign-in"
            visible: sessionModeRadio.checked
        }

        QQC2.Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            opacity: 0.7
            font.italic: true
            text: sessionModeRadio.checked
                ? "Mounted on demand, on first access, with an idle unmount. The password is kept in "
                  + "KWallet — never in a file on disk."
                : "Mounted on demand, on first access, with an idle unmount. Activated by the system boot "
                  + "coordinator, before any user signs in."
        }

        QQC2.Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            opacity: 0.6
            font.italic: true
            text: "To change a network mount, remove it and add it again."
        }

        QQC2.Button {
            Layout.alignment: Qt.AlignRight
            text: "Add"
            enabled: uncField.text.length > 2 && pathField.text.length > 0
            onClicked: {
                if (systemModeRadio.checked) {
                    kcm.actions.addSystemShare(uncField.text, pathField.text, userField.text, domainField.text,
                                              passwordField.text)
                } else {
                    kcm.actions.addShare(uncField.text, pathField.text, userField.text, domainField.text,
                                        passwordField.text, rememberCheck.checked, reconnectCheck.checked)
                }
                dialog.close()
            }
        }
    }
}
