/*
 * mountactions — the operation controller for Add/Delete and the
 * per-share verbs (arm/disarm/mountNow/unmountNow), Session and
 * System alike (plan phase 7). There is no in-place Edit: changing a share's
 * UNC, mount point, credentials, authentication kind, or mode is Delete then
 * Add again (simplification-implementation-plan.md).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Every public method here returns immediately and reports completion via
 * `finished()`. Nothing on the calling (GUI) thread blocks: the per-user lock
 * acquisition, the KAuth call (KAuth::ExecuteJob::exec()) and any KWallet
 * round trip (Wallet::openWallet in Synchronous mode) all run on a worker
 * thread, because all three are unbounded waits on another process — a
 * locked wallet or a slow polkit prompt would otherwise freeze System
 * Settings (plan §10).
 *
 * The per-user lock (Session::UserLock) is acquired first thing on the
 * worker thread and held across the Store snapshot read, the KAuth call and
 * the checked Store/wallet commit that follows — all of it, not just the
 * KAuth call (plan §1.6.4) — and is released only once the worker lambda
 * returns. The GUI-thread continuation only ever Q_EMITs finished(); it does
 * not itself touch Store or KWallet.
 *
 * Mode routing (plan §7.3.2): every method that acts on an *existing*
 * definition (delete/the orphan path-based verb) derives Session vs
 * System fresh from the validated marker via Verify::inspectDefinition(),
 * never from Store and never from a caller-supplied flag, and picks the
 * correspondingly-named helper action (undefine vs undefinesystem, and so
 * on) itself. Only *creating* a definition needs an explicit target mode,
 * since nothing on disk exists yet to derive it from — addSystemShare()'s
 * target System mode is the sole exception, mirroring definesystem's own
 * design §7.1 exception.
 */

#pragma once

#include "unitvalue.h"

#include <QObject>
#include <QString>

namespace Session
{

/**
 * Guest and authenticated inputs must not be mixed (plan §1.7.3): an empty
 * username means guest, which the helper represents as no credential at
 * all, so a non-empty password or domain alongside it cannot be honoured —
 * silently discarding them would surprise a caller who meant to
 * authenticate but mistyped the username. Exposed for testing.
 */
bool guestFieldsConsistent(const QString &username, const QString &domain, const QString &password);

/**
 * Picks the mode-correct helper action name for an operation on an
 * *existing* definition (plan §7.3.2) — e.g. ("undefine", System) ->
 * "undefinesystem". `baseAction` is the Session-flavoured name
 * ("undefine"); the System-flavoured name is always `baseAction +
 * "system"`, which is every such pair in this project's action set. Pure
 * and exposed for testing.
 */
QString modeRoutedAction(const QString &baseAction, UnitValue::CredentialMode mode);

class MountActions : public QObject
{
    Q_OBJECT

public:
    explicit MountActions(QObject *parent = nullptr);

    Q_INVOKABLE void addShare(const QString &unc, const QString &rawMountPoint, const QString &username,
                              const QString &domain, const QString &password, bool remember, bool reconnect);

    /** System mode's create (design §7.1/§8.1): no `remember` (never touches
     *  KWallet) and no `reconnect` (System mode itself means "re-arm at
     *  boot"; design §12's "there is no contradictory per-share reconnect
     *  switch"). Requires authentication; a polkit prompt is expected. */
    Q_INVOKABLE void addSystemShare(const QString &unc, const QString &rawMountPoint, const QString &username,
                                    const QString &domain, const QString &password);

    /** Writes Reconnect=false first for a Session share (plan §6.4), then removes the
     *  definition and the local record. Mode is derived fresh from the validated marker
     *  (plan §7.3.2) — routes to undefine or undefinesystem itself; the caller never
     *  needs to know or guess which. */
    Q_INVOKABLE void deleteShare(const QString &id);

    /** Arms a Session share using a password not (yet) saved in the wallet — "Connect
     *  now". Session only: a System share is always boot-armed and has no equivalent
     *  manual trigger (design §7.4.6). */
    Q_INVOKABLE void connectNow(const QString &id, const QString &password);

    Q_INVOKABLE void disarmShare(const QString &id);
    Q_INVOKABLE void mountNowShare(const QString &id);
    Q_INVOKABLE void unmountNowShare(const QString &id);

    /**
     * Removes a Store record with no backing unit at all (Definition::None) —
     * nothing for the helper to act on, so this is a local KConfig/KWallet
     * removal with no KAuth call (plan §5.4, "removal of the user record").
     */
    Q_INVOKABLE void removeOrphanedRecord(const QString &id);

    /**
     * Path-based removal for a Pair or either-half Partial definition
     * discovered only by scanning the unit tree — no Store record exists
     * for it at all, so there is no id to key off (plan §7.3.1). Mode is
     * derived fresh from the validated marker, exactly like deleteShare().
     */
    Q_INVOKABLE void removeOrphanByPath(const QString &mountPoint);

Q_SIGNALS:
    void started(const QString &id, const QString &kind);
    void finished(const QString &id, const QString &kind, bool success, const QString &message);
};

} // namespace Session
