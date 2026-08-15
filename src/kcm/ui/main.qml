/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

import QtQuick
import QtQuick.Layouts
import QtQuick.Controls as QQC2
import org.kde.kcmutils as KCMUtils

Item {
    id: root
    implicitWidth: 640
    implicitHeight: 480

    KCMUtils.ConfigModule.buttons: KCMUtils.ConfigModule.NoAdditionalButton

    // Mirrors Session::DisplayState (mountmodel.h) — kept in one place so a
    // reorder there is a compile error here too, not a silent UI mismatch.
    readonly property int stateInactive: 0
    readonly property int stateArmed: 1
    readonly property int stateMounted: 2
    readonly property int stateMissingCredentials: 3
    readonly property int stateBroken: 4
    readonly property int stateBusy: 5
    readonly property int stateForeign: 6

    Connections {
        target: kcm.actions
        function onStarted(id, kind) {
            statusLabel.text = ""
            busyIndicator.running = true
        }
        function onFinished(id, kind, success, message) {
            busyIndicator.running = false
            statusLabel.text = message
            statusLabel.color = palette.text
            kcm.shareModel.refresh()
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 8
        spacing: 8

        RowLayout {
            Layout.fillWidth: true

            QQC2.Label {
                text: "Network mounts, mounted on demand — Session shares armed at sign-in, System shares at boot"
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
            }
            QQC2.BusyIndicator {
                id: busyIndicator
                running: false
                visible: running
                implicitWidth: 24
                implicitHeight: 24
            }
            QQC2.Button {
                text: "Add…"
                icon.name: "list-add"
                onClicked: addDialog.openForAdd()
            }
        }

        // design §7.1.8/§7.4.8: global boot-coordinator health, never
        // overriding a specific share's own row state -- shown only when it
        // is actually relevant (a System share exists, or the coordinator
        // itself is unhealthy), not as constant noise for Session-only use.
        QQC2.Pane {
            Layout.fillWidth: true
            visible: kcm.shareModel.hasSystemShares || !kcm.shareModel.bootHealthy
            contentItem: RowLayout {
                QQC2.Label {
                    text: kcm.shareModel.bootHealthy ? "✓" : "⚠"
                }
                QQC2.Label {
                    text: "Boot coordinator: " + kcm.shareModel.bootHealthText
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                }
            }
        }

        QQC2.ScrollView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true

            ListView {
                id: listView
                model: kcm.shareModel
                spacing: 6

                delegate: QQC2.Pane {
                    width: listView.width

                    contentItem: RowLayout {
                        spacing: 8

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 2
                            RowLayout {
                                spacing: 6
                                QQC2.Label {
                                    text: model.mountPoint
                                    font.bold: true
                                    elide: Text.ElideMiddle
                                    Layout.fillWidth: true
                                }
                                QQC2.Label {
                                    visible: model.mode === "system"
                                    text: "System"
                                    opacity: 0.7
                                    font.italic: true
                                }
                                QQC2.Label {
                                    visible: model.drift
                                    text: "⚠ drift"
                                    color: palette.text
                                    opacity: 0.8
                                }
                            }
                            QQC2.Label {
                                text: model.unc + "  —  " + model.stateText
                                      + (model.detail.length > 0 ? ("  (" + model.detail + ")") : "")
                                opacity: 0.7
                                elide: Text.ElideRight
                                Layout.fillWidth: true
                            }
                        }

                        // --- Session-only runtime verbs. Never shown for a
                        // row that requires administrator repair -- those
                        // never offer casual action buttons at all. ---------
                        QQC2.Button {
                            visible: model.hasStoreRecord && model.hasUnitFiles && !model.requiresAdministrator
                                     && model.mode === "session"
                                     && (model.state === stateInactive || model.state === stateMissingCredentials)
                            text: "Connect"
                            onClicked: connectDialog.openFor(model.shareId, model.username)
                        }
                        QQC2.Button {
                            visible: model.hasStoreRecord && model.hasUnitFiles && !model.requiresAdministrator
                                     && model.mode === "session" && model.state === stateArmed
                            text: "Mount now"
                            onClicked: kcm.actions.mountNowShare(model.shareId)
                        }
                        QQC2.Button {
                            visible: model.hasStoreRecord && model.hasUnitFiles && !model.requiresAdministrator
                                     && model.mode === "session" && model.state === stateMounted
                            text: "Unmount now"
                            onClicked: kcm.actions.unmountNowShare(model.shareId)
                        }
                        QQC2.Button {
                            visible: model.hasStoreRecord && model.hasUnitFiles && !model.requiresAdministrator
                                     && model.mode === "session"
                                     && (model.state === stateArmed || model.state === stateMounted
                                         || model.state === stateMissingCredentials)
                            text: "Disarm"
                            onClicked: kcm.actions.disarmShare(model.shareId)
                        }

                        // --- removal, driven directly by the backend's own
                        // actionability booleans (simplification plan §4
                        // action 8) -- QML never reproduces the safety rule
                        // behind them. ---------------------------------------
                        QQC2.Button {
                            visible: model.hasStoreRecord && model.hasUnitFiles && model.canRemoveDefinition
                            text: "Delete"
                            onClicked: deleteConfirm.openFor(model.shareId, model.mountPoint)
                        }
                        QQC2.Button {
                            visible: !model.hasStoreRecord && model.hasUnitFiles && model.canRemoveDefinition
                            text: "Remove"
                            onClicked: kcm.actions.removeOrphanByPath(model.mountPoint)
                        }
                        QQC2.Button {
                            visible: model.hasStoreRecord && model.canRemoveLocalRecord
                            text: "Remove record"
                            onClicked: kcm.actions.removeOrphanedRecord(model.shareId)
                        }

                        // --- administrator-only (Tampered/NotOurs/untrusted-
                        // active): visible, never casually actionable -------
                        QQC2.Label {
                            visible: model.requiresAdministrator
                            text: "Requires administrator repair"
                            opacity: 0.7
                            font.italic: true
                        }
                    }
                }

                QQC2.Label {
                    anchors.centerIn: parent
                    visible: listView.count === 0
                    text: "No network mounts yet — click Add… to create one."
                    opacity: 0.6
                }
            }
        }

        QQC2.Label {
            id: statusLabel
            Layout.fillWidth: true
            visible: text.length > 0
            wrapMode: Text.WordWrap
        }
    }

    ShareAddDialog {
        id: addDialog
        parent: root
        anchors.centerIn: parent
    }

    ConnectDialog {
        id: connectDialog
        parent: root
        anchors.centerIn: parent
    }

    QQC2.Dialog {
        id: deleteConfirm
        parent: root
        anchors.centerIn: parent
        modal: true
        title: "Remove network mount"
        standardButtons: QQC2.Dialog.Yes | QQC2.Dialog.Cancel

        property string targetId: ""

        function openFor(id, mountPoint) {
            targetId = id
            label.text = "Remove the definition for " + mountPoint + "?\n\n"
                       + "If it is currently mounted, it will be disarmed and unmounted first."
            open()
        }

        QQC2.Label {
            id: label
            wrapMode: Text.WordWrap
        }

        onAccepted: kcm.actions.deleteShare(targetId)
    }
}
