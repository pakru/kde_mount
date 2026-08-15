/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "mountactions.h"
#include "helperinvoke.h"
#include "store.h"
#include "userlock.h"
#include "verify.h"

#include <QDir>
#include <QFutureWatcher>
#include <QtConcurrentRun>

#include <memory>
#include <unistd.h>

using Session::HelperOutcome;
using Session::HelperResult;
using Session::UserLock;

namespace
{

/**
 * The stored record must hold the *same* canonical path the helper derives,
 * because that is what every later check compares against.
 *
 * The helper runs QDir::cleanPath() on whatever it is given and writes the
 * result as Where=. Storing the user's raw text instead means a mount point
 * typed with a trailing slash (or a "//" or "/./") is written as one string
 * and compared as another: inspectDefinition's "effective Where= agrees with
 * the canonical mount point" rule then fails, and a perfectly good share is
 * stuck at NeedsAttention forever with every action refused. Normalising once
 * here covers both frontends, since both go through MountActions.
 */
QString canonicalMountPoint(const QString &raw)
{
    return QDir::cleanPath(raw.trimmed());
}

/**
 * What the worker thread hands back to the GUI-thread continuation, which
 * only ever Q_EMITs finished() from it (plan §1.6.4): the per-user lock is
 * acquired first thing on the worker thread and held across the Store
 * snapshot read, the helper call and the checked Store/wallet commit, all of
 * which now happen on the worker thread too — none of it belongs on the GUI
 * thread, and the lock must cover all of it, not just the KAuth call.
 */
struct WorkResult {
    bool success = false;
    QString id; ///< unchanged from the input id, except addShare's newly assigned one
    QString message;
    std::shared_ptr<UserLock> lock;
};

/**
 * Renders one helper outcome for display: `successMessage` on
 * ConfirmedSuccess, the helper's own error text on ConfirmedFailure, and —
 * for Unknown — text that says plainly the result could not be confirmed
 * rather than guessing at either success or failure (plan §1.5.6).
 */
QString describeOutcome(HelperOutcome outcome, const QString &detail, const QString &successMessage)
{
    switch (outcome) {
    case HelperOutcome::ConfirmedSuccess:
        return successMessage;
    case HelperOutcome::ConfirmedFailure:
        return detail;
    case HelperOutcome::Unknown:
        return QStringLiteral("could not confirm the result (%1) — refresh before retrying")
            .arg(detail.isEmpty() ? QStringLiteral("connection to the helper was lost") : detail);
    }
    return detail;
}

/** Derives the real, on-disk mode for an existing definition at
 *  `mountPoint` (plan §7.3.2: "choose Session versus System action from the
 *  validated marker, not Store"). Falls back to Session -- the safer,
 *  passwordless-tier default -- if nothing valid is found; callers that
 *  need to distinguish "no definition at all" from a real Session
 *  definition already check `Verify::Definition` state themselves before or
 *  after calling this. */
UnitValue::CredentialMode deriveMode(const QString &mountPoint)
{
    UnitValue::UnitPaths paths;
    QString pathsError;
    if (!UnitValue::unitPathsFor(mountPoint, &paths, &pathsError)) {
        return UnitValue::CredentialMode::Session;
    }
    const auto def = Verify::inspectDefinition(paths, ::getuid(), mountPoint);
    if (def.state == Verify::Definition::Pair || def.state == Verify::Definition::Partial) {
        return def.mode;
    }
    return UnitValue::CredentialMode::Session;
}

/**
 * Applies the user's password-remember intent for an already-committed
 * share: writes or forgets the wallet entry, then checked-commits
 * `rememberPassword` to match. Never claims a remembered copy exists unless
 * the wallet write actually succeeded (plan §13.3, §1.6.6). Best-effort by
 * design: a failure here does not undo the share/mount change already
 * committed. A `remember` request with no supplied password is left alone —
 * resolving "leave empty to keep the stored password" belongs to plan §1.7,
 * not here. System shares never call this: their credential lives in /etc,
 * never KWallet (design §8.1).
 */
void applyPasswordIntent(const QString &id, const QString &username, const QString &password, bool remember)
{
    if (username.isEmpty()) {
        return; // guest: nothing to remember
    }
    bool wantRemember;
    if (remember && !password.isEmpty()) {
        wantRemember = Store::writePassword(id, password);
    } else if (!remember) {
        Store::removePassword(id);
        wantRemember = false;
    } else {
        return;
    }
    const Store::Snapshot snap = Store::snapshotById(id);
    if (!snap.exists || snap.share.rememberPassword == wantRemember) {
        return;
    }
    Store::Share flagged = snap.share;
    flagged.rememberPassword = wantRemember;
    QString ignored;
    Store::commitShare(flagged, snap.generation, &ignored);
}

} // namespace

namespace Session
{

bool guestFieldsConsistent(const QString &username, const QString &domain, const QString &password)
{
    return !username.isEmpty() || (domain.isEmpty() && password.isEmpty());
}

QString modeRoutedAction(const QString &baseAction, UnitValue::CredentialMode mode)
{
    return (mode == UnitValue::CredentialMode::System) ? (baseAction + QStringLiteral("system")) : baseAction;
}

MountActions::MountActions(QObject *parent) : QObject(parent) { }

void MountActions::addShare(const QString &unc, const QString &rawMountPoint, const QString &username,
                            const QString &domain, const QString &password, bool remember, bool reconnect)
{
    const QString kind = QStringLiteral("add");
    const QString mountPoint = canonicalMountPoint(rawMountPoint);
    Q_EMIT started(QString(), kind);

    if (!guestFieldsConsistent(username, domain, password)) {
        Q_EMIT finished(QString(), kind, false,
                        QStringLiteral("a share with no username is guest, and cannot also have a password or "
                                       "domain"));
        return;
    }

    auto future = QtConcurrent::run([=]() -> WorkResult {
        WorkResult r;
        QString lockError;
        r.lock = UserLock::acquire(&lockError);
        if (!r.lock) {
            r.message = lockError;
            return r;
        }

        const HelperResult defineResult = invokeHelperAction(
            QStringLiteral("define"), {{QStringLiteral("unc"), unc}, {QStringLiteral("path"), mountPoint},
                                      {QStringLiteral("username"), username}});
        if (defineResult.outcome != HelperOutcome::ConfirmedSuccess) {
            // Not confirmed to exist: never write a Store record for it,
            // confirmed-failed or unknown alike (plan §1.5.6) — an unknown
            // define retried blindly is exactly how a duplicate definition
            // happens.
            r.message = describeOutcome(defineResult.outcome, defineResult.message, QString());
            return r;
        }
        if (!UnitValue::isValidShareId(defineResult.id)) {
            r.message = QStringLiteral("the helper returned an invalid share id");
            return r;
        }
        r.id = defineResult.id;

        const HelperResult armResult =
            invokeHelperAction(QStringLiteral("arm"), {{QStringLiteral("path"), mountPoint},
                                                      {QStringLiteral("username"), username},
                                                      {QStringLiteral("domain"), domain},
                                                      {QStringLiteral("password"), password}});

        // The definition itself is confirmed regardless of arm's outcome
        // (plan §1.7: "retain the returned definition ID... report/reconcile
        // activation separately") — a failed or unknown arm still commits
        // the Store record, leaving a visible, unarmed-or-uncertain share
        // rather than discarding a confirmed definition.
        Store::Share share;
        share.id = r.id;
        share.unc = unc;
        share.mountPoint = mountPoint;
        share.username = username;
        share.domain = domain;
        share.reconnect = reconnect;
        share.mode = UnitValue::CredentialMode::Session;
        share.rememberPassword = false;
        QString commitError;
        if (Store::commitShare(share, /*expectedGeneration=*/0, &commitError) != Store::CommitResult::Ok) {
            r.message = QStringLiteral(
                            "the definition was created, but could not be saved locally (%1) — it exists on this "
                            "machine but will not appear here until this is resolved")
                            .arg(commitError);
            return r;
        }
        if (!username.isEmpty() && remember && !password.isEmpty()) {
            applyPasswordIntent(r.id, username, password, remember);
        }

        r.success = true;
        switch (armResult.outcome) {
        case HelperOutcome::ConfirmedSuccess:
            r.message = QStringLiteral("Share added and armed");
            break;
        case HelperOutcome::ConfirmedFailure:
            r.message = QStringLiteral("Share added, but could not be armed: %1").arg(armResult.message);
            break;
        case HelperOutcome::Unknown:
            r.message = QStringLiteral(
                            "Share added; whether it was armed could not be confirmed (%1) — refresh before retrying")
                            .arg(armResult.message.isEmpty() ? QStringLiteral("connection to the helper was lost")
                                                             : armResult.message);
            break;
        }
        return r;
    });

    auto *watcher = new QFutureWatcher<WorkResult>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, kind]() {
        const WorkResult r = watcher->future().result();
        watcher->deleteLater();
        // r.lock is destroyed here, releasing the per-user lock only after
        // every Store/wallet commit above.
        Q_EMIT finished(r.id, kind, r.success, r.message);
    });
    watcher->setFuture(future);
}

void MountActions::addSystemShare(const QString &unc, const QString &rawMountPoint, const QString &username,
                                  const QString &domain, const QString &password)
{
    const QString kind = QStringLiteral("addSystem");
    const QString mountPoint = canonicalMountPoint(rawMountPoint);
    Q_EMIT started(QString(), kind);

    if (!guestFieldsConsistent(username, domain, password)) {
        Q_EMIT finished(QString(), kind, false,
                        QStringLiteral("a share with no username is guest, and cannot also have a password or "
                                       "domain"));
        return;
    }

    auto future = QtConcurrent::run([=]() -> WorkResult {
        WorkResult r;
        QString lockError;
        r.lock = UserLock::acquire(&lockError);
        if (!r.lock) {
            r.message = lockError;
            return r;
        }

        const HelperResult defineResult = invokeHelperAction(
            QStringLiteral("definesystem"),
            {{QStringLiteral("unc"), unc}, {QStringLiteral("path"), mountPoint},
             {QStringLiteral("username"), username}, {QStringLiteral("domain"), domain},
             {QStringLiteral("password"), password}});
        if (defineResult.outcome != HelperOutcome::ConfirmedSuccess) {
            r.message = describeOutcome(defineResult.outcome, defineResult.message, QString());
            return r;
        }
        if (!UnitValue::isValidShareId(defineResult.id)) {
            r.message = QStringLiteral("the helper returned an invalid share id");
            return r;
        }
        r.id = defineResult.id;

        // definesystem arms immediately, as its own last step (design
        // §6.3a) — unlike Session's addShare(), there is no separate arm
        // step to call here, and no wallet interaction ever (design §8.1).
        Store::Share share;
        share.id = r.id;
        share.unc = unc;
        share.mountPoint = mountPoint;
        share.username = username;
        share.domain = domain;
        share.reconnect = false;
        share.mode = UnitValue::CredentialMode::System;
        share.rememberPassword = false;
        QString commitError;
        if (Store::commitShare(share, /*expectedGeneration=*/0, &commitError) != Store::CommitResult::Ok) {
            r.message = QStringLiteral(
                            "the definition was created, but could not be saved locally (%1) — it exists on this "
                            "machine but will not appear here until this is resolved")
                            .arg(commitError);
            return r;
        }

        r.success = true;
        r.message = defineResult.activated
            ? QStringLiteral("Share added and armed")
            : QStringLiteral("Share added, but could not be armed — check the boot coordinator");
        return r;
    });

    auto *watcher = new QFutureWatcher<WorkResult>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, kind]() {
        const WorkResult r = watcher->future().result();
        watcher->deleteLater();
        Q_EMIT finished(r.id, kind, r.success, r.message);
    });
    watcher->setFuture(future);
}

void MountActions::deleteShare(const QString &id)
{
    const QString kind = QStringLiteral("delete");
    Q_EMIT started(id, kind);

    auto future = QtConcurrent::run([=]() -> WorkResult {
        WorkResult r;
        r.id = id;
        QString lockError;
        r.lock = UserLock::acquire(&lockError);
        if (!r.lock) {
            r.message = lockError;
            return r;
        }

        const Store::Snapshot snap = Store::snapshotById(id);
        if (!snap.exists) {
            r.message = QStringLiteral("no such share");
            return r;
        }
        const Store::Share &share = snap.share;
        quint64 generation = snap.generation;
        const UnitValue::CredentialMode mode = deriveMode(share.mountPoint);

        // Reconnect=false first, synced, before anything else: no later
        // failure can resurrect this share at the next sign-in (plan §6.4).
        // Session only -- System has no reconnect concept to disable.
        bool reconnectDisabled = false;
        if (mode == UnitValue::CredentialMode::Session && share.reconnect) {
            Store::Share updated = share;
            updated.reconnect = false;
            QString commitError;
            if (Store::commitShare(updated, generation, &commitError) != Store::CommitResult::Ok) {
                r.message = QStringLiteral("could not disable reconnect before removal: %1").arg(commitError);
                return r;
            }
            ++generation;
            reconnectDisabled = true;
        }

        const HelperResult undefineResult = invokeHelperAction(
            modeRoutedAction(QStringLiteral("undefine"), mode), {{QStringLiteral("path"), share.mountPoint}});
        if (undefineResult.outcome != HelperOutcome::ConfirmedSuccess) {
            // Neither a confirmed failure nor an unknown result may remove
            // the Store record — an unknown removal that actually succeeded
            // would otherwise leave a root definition with no local record
            // pointing at it (plan §1.5.6).
            r.message = QStringLiteral("could not fully remove %1: %2%3")
                            .arg(share.mountPoint,
                                 describeOutcome(undefineResult.outcome, undefineResult.message, QString()),
                                 reconnectDisabled ? QStringLiteral(" — Reconnect has been disabled; retry removal later")
                                                   : QStringLiteral(" — retry removal later"));
            return r;
        }
        r.success = Store::removeShare(id);
        r.message = r.success ? QStringLiteral("Removed")
                              : QStringLiteral("removed the definition, but the local record could not be fully "
                                               "cleared — retry from this list");
        return r;
    });

    auto *watcher = new QFutureWatcher<WorkResult>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, kind]() {
        const WorkResult r = watcher->future().result();
        watcher->deleteLater();
        Q_EMIT finished(r.id, kind, r.success, r.message);
    });
    watcher->setFuture(future);
}

void MountActions::connectNow(const QString &id, const QString &password)
{
    const QString kind = QStringLiteral("connect");
    Q_EMIT started(id, kind);

    auto future = QtConcurrent::run([=]() -> WorkResult {
        WorkResult r;
        r.id = id;
        QString lockError;
        r.lock = UserLock::acquire(&lockError);
        if (!r.lock) {
            r.message = lockError;
            return r;
        }
        const Store::Snapshot snap = Store::snapshotById(id);
        if (!snap.exists) {
            r.message = QStringLiteral("no such share");
            return r;
        }
        const HelperResult armResult = invokeHelperAction(QStringLiteral("arm"),
                                                          {{QStringLiteral("path"), snap.share.mountPoint},
                                                           {QStringLiteral("username"), snap.share.username},
                                                           {QStringLiteral("domain"), snap.share.domain},
                                                           {QStringLiteral("password"), password}});
        r.success = (armResult.outcome == HelperOutcome::ConfirmedSuccess);
        r.message = describeOutcome(armResult.outcome, armResult.message, QStringLiteral("Armed"));
        return r;
    });

    auto *watcher = new QFutureWatcher<WorkResult>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, kind]() {
        const WorkResult r = watcher->future().result();
        watcher->deleteLater();
        Q_EMIT finished(r.id, kind, r.success, r.message);
    });
    watcher->setFuture(future);
}

void MountActions::disarmShare(const QString &id)
{
    const QString kind = QStringLiteral("disarm");
    Q_EMIT started(id, kind);
    auto future = QtConcurrent::run([=]() -> WorkResult {
        WorkResult r;
        r.id = id;
        QString lockError;
        r.lock = UserLock::acquire(&lockError);
        if (!r.lock) {
            r.message = lockError;
            return r;
        }
        const Store::Snapshot snap = Store::snapshotById(id);
        if (!snap.exists) {
            r.message = QStringLiteral("no such share");
            return r;
        }
        const HelperResult disarmResult =
            invokeHelperAction(QStringLiteral("disarm"), {{QStringLiteral("path"), snap.share.mountPoint}});
        r.success = (disarmResult.outcome == HelperOutcome::ConfirmedSuccess);
        r.message = describeOutcome(disarmResult.outcome, disarmResult.message, QStringLiteral("Disarmed"));
        return r;
    });
    auto *watcher = new QFutureWatcher<WorkResult>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, kind]() {
        const WorkResult r = watcher->future().result();
        watcher->deleteLater();
        Q_EMIT finished(r.id, kind, r.success, r.message);
    });
    watcher->setFuture(future);
}

void MountActions::mountNowShare(const QString &id)
{
    const QString kind = QStringLiteral("mountNow");
    Q_EMIT started(id, kind);
    auto future = QtConcurrent::run([=]() -> WorkResult {
        WorkResult r;
        r.id = id;
        QString lockError;
        r.lock = UserLock::acquire(&lockError);
        if (!r.lock) {
            r.message = lockError;
            return r;
        }
        const Store::Snapshot snap = Store::snapshotById(id);
        if (!snap.exists) {
            r.message = QStringLiteral("no such share");
            return r;
        }
        const HelperResult mountResult =
            invokeHelperAction(QStringLiteral("mountnow"), {{QStringLiteral("path"), snap.share.mountPoint}});
        r.success = (mountResult.outcome == HelperOutcome::ConfirmedSuccess);
        r.message = describeOutcome(mountResult.outcome, mountResult.message, QStringLiteral("Mounted"));
        return r;
    });
    auto *watcher = new QFutureWatcher<WorkResult>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, kind]() {
        const WorkResult r = watcher->future().result();
        watcher->deleteLater();
        Q_EMIT finished(r.id, kind, r.success, r.message);
    });
    watcher->setFuture(future);
}

void MountActions::unmountNowShare(const QString &id)
{
    const QString kind = QStringLiteral("unmountNow");
    Q_EMIT started(id, kind);
    auto future = QtConcurrent::run([=]() -> WorkResult {
        WorkResult r;
        r.id = id;
        QString lockError;
        r.lock = UserLock::acquire(&lockError);
        if (!r.lock) {
            r.message = lockError;
            return r;
        }
        const Store::Snapshot snap = Store::snapshotById(id);
        if (!snap.exists) {
            r.message = QStringLiteral("no such share");
            return r;
        }
        const HelperResult unmountResult =
            invokeHelperAction(QStringLiteral("unmountnow"), {{QStringLiteral("path"), snap.share.mountPoint}});
        r.success = (unmountResult.outcome == HelperOutcome::ConfirmedSuccess);
        r.message = describeOutcome(unmountResult.outcome, unmountResult.message, QStringLiteral("Unmounted"));
        return r;
    });
    auto *watcher = new QFutureWatcher<WorkResult>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, kind]() {
        const WorkResult r = watcher->future().result();
        watcher->deleteLater();
        Q_EMIT finished(r.id, kind, r.success, r.message);
    });
    watcher->setFuture(future);
}

void MountActions::removeOrphanedRecord(const QString &id)
{
    const QString kind = QStringLiteral("removeRecord");
    Q_EMIT started(id, kind);
    // No helper call: Definition::None means there is nothing on the
    // privileged side for it to act on (plan §5.4). It is still a Store and
    // KWallet mutation, so it follows the same worker-thread/UserLock rule as
    // every other client operation.
    auto future = QtConcurrent::run([=]() -> WorkResult {
        WorkResult r;
        r.id = id;
        QString lockError;
        r.lock = UserLock::acquire(&lockError);
        if (!r.lock) {
            r.message = lockError;
            return r;
        }
        r.success = Store::removeShare(id);
        r.message = r.success ? QStringLiteral("Removed")
                              : QStringLiteral("could not confirm KWallet/local-record cleanup — retry");
        return r;
    });
    auto *watcher = new QFutureWatcher<WorkResult>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, kind]() {
        const WorkResult r = watcher->future().result();
        watcher->deleteLater();
        Q_EMIT finished(r.id, kind, r.success, r.message);
    });
    watcher->setFuture(future);
}

void MountActions::removeOrphanByPath(const QString &mountPoint)
{
    const QString kind = QStringLiteral("removeOrphan");
    Q_EMIT started(QString(), kind);
    auto future = QtConcurrent::run([=]() -> WorkResult {
        WorkResult r;
        QString lockError;
        r.lock = UserLock::acquire(&lockError);
        if (!r.lock) {
            r.message = lockError;
            return r;
        }
        UnitValue::UnitPaths paths;
        QString pathsError;
        if (!UnitValue::unitPathsFor(mountPoint, &paths, &pathsError)) {
            r.message = pathsError;
            return r;
        }
        const auto def = Verify::inspectDefinition(paths, ::getuid(), mountPoint);
        if (def.state != Verify::Definition::Pair && def.state != Verify::Definition::Partial) {
            r.message = QStringLiteral("no owned definition exists at %1").arg(mountPoint);
            return r;
        }
        const HelperResult result = invokeHelperAction(modeRoutedAction(QStringLiteral("undefine"), def.mode),
                                                       {{QStringLiteral("path"), mountPoint}});
        r.success = (result.outcome == HelperOutcome::ConfirmedSuccess);
        r.message = describeOutcome(result.outcome, result.message, QStringLiteral("Removed"));
        return r;
    });
    auto *watcher = new QFutureWatcher<WorkResult>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, kind]() {
        const WorkResult r = watcher->future().result();
        watcher->deleteLater();
        Q_EMIT finished(r.id, kind, r.success, r.message);
    });
    watcher->setFuture(future);
}


} // namespace Session
