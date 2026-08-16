/*
 * The Dolphin service-menu window: "Mount as Network Drive".
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * A thin host around the shared ShareForm, which is the same file the KCM
 * embeds — the two front ends now differ only in chrome and in what each
 * knows up front. Here the share is fixed (it came from the smb:// URL
 * Dolphin was invoked on) and, when that share is already saved, this window
 * offers removal instead of an add form, matching the original service-menu
 * interaction. Editing a saved share remains a KCM job.
 */

import QtQuick
import QtQuick.Layouts
import QtQuick.Controls as QQC2

QQC2.ApplicationWindow {
    id: root

    readonly property bool existing: backend.existingId.length > 0
    property bool busy: false

    visible: true
    title: "Mount as Network Drive"
    width: 560
    height: existing ? 260 : 620
    minimumWidth: 420

    Connections {
        target: backend.actions
        function onFinished(id, kind, success, message) {
            root.busy = false
            if (success) {
                resultDialog.title = kind === "add" ? "Added" : "Removed"
                resultDialog.closeWhenDone = true
            } else {
                resultDialog.title = "Failed"
                resultDialog.closeWhenDone = false
            }
            resultText.text = message
            resultDialog.open()
        }
    }

    QQC2.Dialog {
        id: resultDialog
        property bool closeWhenDone: false
        anchors.centerIn: parent
        width: Math.min(root.width - 40, 460)
        modal: true
        standardButtons: QQC2.Dialog.Ok
        // Escape / click-outside must not silently skip the quit-on-success
        // path below -- Popup.close() (which both of those trigger) does not
        // emit accepted(), so OK has to be the only way out of this dialog.
        closePolicy: QQC2.Popup.NoAutoClose
        onAccepted: if (closeWhenDone) { Qt.quit() }

        QQC2.Label {
            id: resultText
            width: parent.width
            wrapMode: Text.WordWrap
        }
    }

    QQC2.Dialog {
        id: confirmRemove
        anchors.centerIn: parent
        width: Math.min(root.width - 40, 460)
        modal: true
        title: "Remove this saved share?"
        standardButtons: QQC2.Dialog.Ok | QQC2.Dialog.Cancel
        onAccepted: {
            root.busy = true
            backend.removeExisting()
        }

        QQC2.Label {
            width: parent.width
            wrapMode: Text.WordWrap
            text: "If it is currently mounted, it will be unmounted first. The mount point directory "
                + "itself is left in place."
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 12
        spacing: 8

        // --- already-saved share: report and offer removal only -------------
        QQC2.Label {
            visible: root.existing
            wrapMode: Text.WordWrap
            font.bold: true
            text: backend.unc
            Layout.fillWidth: true
        }
        QQC2.Label {
            visible: root.existing
            wrapMode: Text.WordWrap
            text: "This share is already saved, at " + backend.existingMountPoint
                + " (" + backend.existingStateText + ")."
            Layout.fillWidth: true
        }
        QQC2.Label {
            visible: root.existing
            wrapMode: Text.WordWrap
            opacity: 0.7
            font.italic: true
            text: "To change settings for a saved share, use System Settings → Network Mounts. "
                + "Remove below deletes it entirely."
            Layout.fillWidth: true
        }

        // --- new share: the shared form --------------------------------------
        ShareForm {
            id: form
            visible: !root.existing
            actions: backend.actions
            fixedUnc: backend.unc
            mountPoint: backend.suggestedPath
            username: backend.suggestedUser
            Layout.fillWidth: true
            onSubmitted: root.busy = true
        }

        Item { Layout.fillHeight: true }

        RowLayout {
            Layout.fillWidth: true
            Item { Layout.fillWidth: true }
            QQC2.Button {
                text: "Cancel"
                enabled: !root.busy
                onClicked: Qt.quit()
            }
            QQC2.Button {
                visible: !root.existing
                text: "Mount"
                enabled: form.canSubmit && !root.busy
                onClicked: form.submit()
            }
            QQC2.Button {
                visible: root.existing
                text: "Remove"
                enabled: !root.busy
                onClicked: confirmRemove.open()
            }
        }
    }
}
