/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include <QDialog>
#include <QString>

class QCheckBox;
class QLabel;
class QLineEdit;
class QPushButton;

namespace Session
{
class MountActions;
}

/**
 * The "Mount as Network Drive" dialog.
 *
 * Unprivileged. Anything it validates is for the user's benefit only — the KAuth
 * helper re-checks everything, because a hostile process can call the action
 * directly and never come through here.
 *
 * A lightweight entry point, not the management surface: it can add a new
 * share or fully remove an existing one, matching the original service-menu
 * interaction. Editing a saved share's settings without removing it is a KCM
 * job (kcm_nasmount).
 */
class MountDialog : public QDialog
{
    Q_OBJECT

public:
    MountDialog(const QString &unc, const QString &suggestedUser, QWidget *parent = nullptr);

    /** Turns an smb:// URL into a CIFS UNC path, or reports why it cannot. */
    static bool parseSmbUrl(const QString &raw, QString *unc, QString *user, QString *error);

private Q_SLOTS:
    void browse();
    void doMount();
    void doUnmount();
    void onActionFinished(const QString &id, const QString &kind, bool success, const QString &message);

private:
    QString m_unc;
    QString m_existingId; ///< non-empty when this share is already saved
    QString m_stateText;  ///< current display state, for the existing-share note

    Session::MountActions *m_actions = nullptr;

    QLineEdit *m_path = nullptr;
    QLineEdit *m_user = nullptr;
    QLineEdit *m_password = nullptr;
    QLineEdit *m_domain = nullptr;
    QCheckBox *m_remember = nullptr;
    QCheckBox *m_reconnect = nullptr;
    QLabel *m_note = nullptr;
    QPushButton *m_mountButton = nullptr;
    QPushButton *m_unmountButton = nullptr;
};
