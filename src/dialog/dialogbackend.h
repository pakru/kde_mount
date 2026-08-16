/*
 * DialogBackend — the service menu window's C++ side: everything the shared
 * QML form cannot know on its own.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Replaces the old QWidgets MountDialog. The form itself is now
 * ShareForm.qml, shared verbatim with the KCM, so what is left here is only
 * the service-menu-specific context: the UNC the invocation was for, whether
 * that share is already saved (in which case the window offers removal
 * instead of an add form), and the outcome of the asynchronous action.
 *
 * Unprivileged, like the dialog it replaces. Anything it decides is for the
 * user's benefit only — the KAuth helper re-checks everything.
 */

#pragma once

// Full definition, not a forward declaration: Session::MountActions is
// exposed to QML as a pointer Q_PROPERTY, and Qt's metatype system requires
// the pointee to be complete.
#include "mountactions.h"

#include <QObject>
#include <QString>

class DialogBackend : public QObject
{
    Q_OBJECT

    Q_PROPERTY(QString unc READ unc CONSTANT)
    Q_PROPERTY(QString suggestedUser READ suggestedUser CONSTANT)
    Q_PROPERTY(QString suggestedPath READ suggestedPath CONSTANT)
    /** Empty unless this share is already saved; then the window shows the
     *  existing-share note and a Remove button rather than the add form. */
    Q_PROPERTY(QString existingId READ existingId CONSTANT)
    Q_PROPERTY(QString existingMountPoint READ existingMountPoint CONSTANT)
    Q_PROPERTY(QString existingStateText READ existingStateText CONSTANT)
    Q_PROPERTY(Session::MountActions *actions READ actions CONSTANT)

public:
    DialogBackend(const QString &unc, const QString &suggestedUser, QObject *parent = nullptr);

    QString unc() const { return m_unc; }
    QString suggestedUser() const { return m_suggestedUser; }
    QString suggestedPath() const { return m_suggestedPath; }
    QString existingId() const { return m_existingId; }
    QString existingMountPoint() const { return m_existingMountPoint; }
    QString existingStateText() const { return m_existingStateText; }
    Session::MountActions *actions() const { return m_actions; }

    Q_INVOKABLE void removeExisting();

private:
    QString m_unc;
    QString m_suggestedUser;
    QString m_suggestedPath;
    QString m_existingId;
    QString m_existingMountPoint;
    QString m_existingStateText;
    Session::MountActions *m_actions = nullptr;
};
