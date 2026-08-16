/*
 * ShareForm — the add-a-share form, shared verbatim by both front ends: the
 * KCM wraps it in a QQC2.Dialog, nasmount-dialog wraps it in a window.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file must stay host-agnostic. It never references `kcm` (or any other
 * host context object) — the MountActions instance arrives through the
 * `actions` property, and every other host difference is a property too. That
 * is what lets one definition serve both entry points; a single `kcm.` here
 * would silently make it KCM-only again, which is exactly how the two forms
 * drifted apart before (the service menu could not create System shares at
 * all, because its separate copy predated the mode selector).
 *
 * Validation here is convenience only. UnitSpec re-validates every field in
 * the privileged helper, which is the boundary that actually matters.
 */

import QtQuick
import QtQuick.Layouts
import QtQuick.Controls as QQC2
import QtQuick.Dialogs as QtDialogs

ColumnLayout {
    id: form

    /** Session::MountActions, injected by the host. */
    property var actions: null

    /** When non-empty the share is fixed (the service menu already knows it
     *  from the smb:// URL it was invoked on) and is shown as a header rather
     *  than an editable field. Empty means the user types it, as in the KCM. */
    property string fixedUnc: ""

    property alias mountPoint: pathField.text
    property alias username: userField.text

    /** The share this form will actually submit. */
    readonly property string effectiveUnc: fixedUnc.length > 0 ? fixedUnc : uncField.text

    readonly property bool canSubmit: effectiveUnc.length > 2 && pathField.text.length > 0

    /** Emitted after a submit has been handed to `actions`; the host decides
     *  what closing means for it (a dialog closes, a window waits for the
     *  finished() signal so it can report the outcome). */
    signal submitted()

    spacing: 6

    QtDialogs.FolderDialog {
        id: folderDialog
        onAccepted: pathField.text = selectedFolder.toString().replace("file://", "")
    }

    QQC2.Label {
        visible: form.fixedUnc.length > 0
        text: form.fixedUnc
        font.bold: true
        elide: Text.ElideMiddle
        Layout.fillWidth: true
    }

    QQC2.Label {
        visible: form.fixedUnc.length === 0
        text: "Share (//host/share[/subdir]):"
    }
    QQC2.TextField {
        id: uncField
        visible: form.fixedUnc.length === 0
        Layout.fillWidth: true
    }

    QQC2.Label { text: "Mount point:" }
    RowLayout {
        Layout.fillWidth: true
        QQC2.TextField {
            id: pathField
            placeholderText: "/home/you/ShareName"
            Layout.fillWidth: true
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
            }
        }
    }

    QQC2.Label { text: "Password:" }
    QQC2.TextField {
        id: passwordField
        echoMode: TextInput.Password
        enabled: userField.text.length > 0
        Layout.fillWidth: true
    }

    QQC2.Label { text: "Domain (optional):" }
    QQC2.TextField {
        id: domainField
        enabled: userField.text.length > 0
        Layout.fillWidth: true
    }

    QQC2.Label {
        wrapMode: Text.WordWrap
        visible: userField.text.length > 0
        text: "This share's password is stored in a file on this computer, readable by administrators. "
            + "Same as a hand-written system mount."
        Layout.fillWidth: true
        Layout.topMargin: 6
    }

    QQC2.Label {
        wrapMode: Text.WordWrap
        opacity: 0.7
        font.italic: true
        text: "Mounted on demand, on first access, with an idle unmount. Activated at startup, before "
            + "anyone signs in — so it is available again after a reboot without signing in first."
        Layout.fillWidth: true
    }

    QQC2.Label {
        wrapMode: Text.WordWrap
        opacity: 0.6
        font.italic: true
        text: "To change a network mount, remove it and add it again."
        Layout.fillWidth: true
    }

    function reset() {
        uncField.text = "//"
        pathField.text = ""
        userField.text = ""
        domainField.text = ""
        passwordField.text = ""
    }

    // One lifecycle, no per-share switches: saving a share means it is armed
    // at boot and mounts on first access. There is deliberately no "arm at
    // sign-in" or mode choice to make -- asking to mount a share *is* asking
    // for it to be there after a reboot.
    function submit() {
        form.actions.addShare(effectiveUnc, pathField.text, userField.text, domainField.text,
                              passwordField.text)
        form.submitted()
    }
}
