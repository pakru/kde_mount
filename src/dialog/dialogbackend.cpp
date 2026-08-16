/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "dialogbackend.h"
#include "mountactions.h"
#include "smburl.h"
#include "store.h"

DialogBackend::DialogBackend(const QString &unc, const QString &suggestedUser, QObject *parent)
    : QObject(parent)
    , m_unc(unc)
    , m_suggestedUser(suggestedUser)
    , m_actions(new Session::MountActions(this))
{
    // Prefer whatever was saved for this share last time; only fall back to a
    // suggested path for a share that is genuinely new.
    for (const Store::Share &s : Store::shares()) {
        if (s.unc == unc) {
            m_existingId = s.id;
            m_existingMountPoint = s.mountPoint;
            m_suggestedUser = s.username;
            break;
        }
    }
    if (!m_existingId.isEmpty()) {
        m_existingStateText = Dialog::SmbUrl::describeState(m_existingMountPoint);
        m_suggestedPath = m_existingMountPoint;
    } else {
        m_suggestedPath = Dialog::SmbUrl::suggestMountpoint(unc);
    }
}

void DialogBackend::removeExisting()
{
    if (!m_existingId.isEmpty()) {
        m_actions->deleteShare(m_existingId);
    }
}
