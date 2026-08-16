/*
 * smburl — the service menu's pure input handling: turning the smb:// URL
 * Dolphin substitutes into a validated CIFS UNC, and the small presentation
 * helpers that go with it.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Split out of the dialog itself so it is testable without a GUI (smburl_test)
 * and so it survives the front end being rewritten: it moved here intact when
 * the QWidgets dialog was replaced by the shared QML form. None of it is a
 * security boundary — the KAuth helper re-validates every field regardless,
 * because a hostile process can invoke the action directly and never come
 * through this code at all — but it is the parsing most likely to meet
 * genuinely odd input, since Dolphin substitutes %u with the URL *as
 * displayed*, spaces and all.
 */

#pragma once

#include <QString>

namespace Dialog::SmbUrl
{

/** Turns an smb:// URL into a CIFS UNC path, or reports why it cannot.
 *  Rejects ports, passwords, queries, fragments, IPv6 hosts, server-only
 *  URLs and control characters. */
bool parse(const QString &raw, QString *unc, QString *user, QString *error);

/**
 * Suggests ~/<ShareName> for the share being mounted.
 *
 * The share's own name is kept as-is rather than upper-cased or filed under a
 * fixed parent folder: those are personal conventions, and the name the server
 * already uses is the least surprising default.
 */
QString suggestMountpoint(const QString &unc);

/** Coarse state text for a saved share, without needing the full KCM model. */
QString describeState(const QString &mountPoint);

} // namespace Dialog::SmbUrl
