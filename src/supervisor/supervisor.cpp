/*
 * nasmount-supervisor — the session supervisor (plan §7.1).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * A systemd --user service, PartOf=graphical-session.target,
 * RemainAfterExit=yes, always active for the graphical session. `--start` is
 * ExecStart=: it arms every share marked Reconnect=true. `--stop` is
 * ExecStopPost=, not ExecStop= — ExecStop is skipped when ExecStart fails or
 * times out, which would strand shares armed earlier in the same run. Both
 * must be idempotent: `--stop` also runs after a startup sweep and after a
 * user-manager restart mid-session, when some units may already be disarmed.
 *
 * QCoreApplication only: this is a oneshot binary, not a GUI, so a blocking
 * KAuth call here does not freeze anything.
 *
 * **Both modes exit 0 even when individual shares fail.** A per-share failure
 * is an ordinary operational condition, not a broken supervisor: the NAS is
 * unreachable, the laptop is away from that network, KWallet is not unlocked
 * yet at sign-in (which plan §7.1 explicitly requires be a skip, not a
 * prompt), or a definition was removed while its config record lingered.
 * Exiting non-zero for any of those puts a Type=oneshot RemainAfterExit=yes
 * unit into `failed` rather than `active`, which breaks the one invariant the
 * supervisor exists to provide — that it is *always active for the graphical
 * session*, so ExecStopPost is there to disarm everything at logout. Failures
 * are reported to the journal instead.
 */

#include "helperinvoke.h"
#include "store.h"
#include "userlock.h"
#include "verify.h"

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QSet>
#include <QTextStream>
#include <QThread>

namespace
{

/** How long to wait for KWallet to be unlocked before giving up on shares
 *  that need a stored password. Bounded so a machine with no wallet, or a
 *  user who cancels the unlock, does not leave this unit activating forever;
 *  the service's own TimeoutStartSec= is comfortably longer. */
constexpr int WalletWaitSeconds = 90;
constexpr int WalletPollMs = 250;

/**
 * Waits for KWallet to become unlocked, without ever asking it to unlock.
 *
 * Checking once and giving up does not work: on a Plasma login the wallet is
 * unlocked by pam_kwallet, driven by plasma-kwallet-pam.service, which is
 * Type=simple — systemd reports it "Started" as soon as pam_kwallet_init
 * spawns, but the actual unlock happens afterwards, asynchronously, once that
 * process has piped the login password to kwalletd over a socket. Measured on
 * a real boot, this supervisor ran 2.5s *after* that service started and the
 * wallet still was not open, so an After= ordering dependency does not fix it
 * either. Polling for the state we actually care about is what does.
 *
 * This preserves the "never prompt" rule of plan §7.1: it only ever observes
 * Wallet::isOpen(), and never calls openWallet() on a locked wallet.
 */
bool waitForWallet(QTextStream &out)
{
    if (Store::walletIsOpen()) {
        return true;
    }
    out << "waiting up to " << WalletWaitSeconds << "s for KWallet to be unlocked..." << Qt::endl;

    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < WalletWaitSeconds * 1000) {
        QThread::msleep(WalletPollMs);
        if (Store::walletIsOpen()) {
            out << "KWallet unlocked after " << (timer.elapsed() / 1000.0) << "s." << Qt::endl;
            return true;
        }
    }
    return false;
}

/**
 * Owned Session units whose automount is already Active with a Trusted
 * (matching) recorded instance id (design §12.1/§6.4): most often a
 * supervisor restart after a user-manager crash, where the automount and its
 * `/run` credential survived even though this process did not arm them.
 * `arm()` itself would refuse such a unit ("already armed"), so these ids are
 * accepted as-is below rather than re-armed or re-recorded. System units are
 * never included -- they are nasmount-boot's responsibility, never armed or
 * inspected for this purpose here (plan §5.5).
 */
QSet<QString> alreadyArmedSessionIds()
{
    QSet<QString> ids;
    for (const Verify::OwnedUnit &unit : Verify::enumerateOwnedUnits(::getuid())) {
        if (unit.mode != UnitValue::CredentialMode::Session || unit.state != Verify::Definition::Pair) {
            continue;
        }
        const Verify::RuntimeSnapshot snap = Verify::inspectRuntime(unit.unitName, unit.mountPoint, unit.what);
        if (snap.automount == Verify::AutomountState::Active
            && snap.activationTrust == Verify::ActivationTrust::Trusted) {
            ids.insert(unit.id);
        }
    }
    return ids;
}

int runStart()
{
    QTextStream out(stdout);
    int failures = 0;

    // Reconcile owned Session units via the structured both-half inventory
    // (design §12.2), not the Store snapshot alone: Store cannot prove which
    // trigger instance, if any, is already trusted.
    const QSet<QString> alreadyArmed = alreadyArmedSessionIds();

    // Only wait if something actually needs a password and is not already
    // armed -- an all-guest, System-only, or already-reconciled configuration
    // must not sit here for 90s (plan §5.6).
    bool needsWallet = false;
    for (const Store::Share &share : Store::shares()) {
        if (share.reconnect && share.mode == UnitValue::CredentialMode::Session && !share.username.isEmpty()
            && !alreadyArmed.contains(share.id)) {
            needsWallet = true;
            break;
        }
    }

    const bool walletOpen = needsWallet ? waitForWallet(out) : Store::walletIsOpen();
    if (needsWallet && !walletOpen) {
        out << "KWallet was not unlocked within " << WalletWaitSeconds
            << "s; shares needing a stored password are skipped. Arm them from "
               "System Settings -> Network Mounts once the wallet is open."
            << Qt::endl;
    }

    for (const Store::Share &share : Store::shares()) {
        if (!share.reconnect || share.mode != UnitValue::CredentialMode::Session) {
            continue; // System rows are boot-managed, never this process's job (plan §5.5)
        }
        if (alreadyArmed.contains(share.id)) {
            out << "already armed, matching id, left as-is: " << share.mountPoint << Qt::endl;
            continue;
        }
        QString password;
        if (!share.username.isEmpty()) {
            if (!walletOpen || !Store::readPassword(share.id, &password, /*onlyIfUnlocked=*/true)) {
                out << "no stored password, skipping: " << share.mountPoint << Qt::endl;
                ++failures;
                continue;
            }
        }

        QString lockError;
        auto lock = Session::UserLock::acquire(&lockError);
        if (!lock) {
            out << "FAILED: " << share.mountPoint << " -> " << lockError << Qt::endl;
            ++failures;
            continue;
        }
        const Session::HelperResult result = Session::invokeHelperAction(
            QStringLiteral("arm"), {{QStringLiteral("path"), share.mountPoint},
                                   {QStringLiteral("username"), share.username},
                                   {QStringLiteral("domain"), share.domain},
                                   {QStringLiteral("password"), password}});
        if (result.outcome == Session::HelperOutcome::ConfirmedSuccess) {
            out << "armed: " << share.mountPoint << Qt::endl;
        } else {
            out << "FAILED: " << share.mountPoint << " -> " << result.message << Qt::endl;
            ++failures;
        }
    }
    return failures;
}

int runStop()
{
    QTextStream out(stdout);
    int failures = 0;

    // Enumerate every unit this uid owns, not the Store snapshot: toggling or
    // deleting a config record must not hide an armed unit from teardown
    // (plan §7.1). Tampered entries are skipped: their fields cannot be
    // trusted enough to act on, and attempting disarm() on one would only add
    // log noise. System units are left entirely alone -- their triggers,
    // credentials and records are nasmount-boot's, not torn down at logout
    // (plan §5.7/design §12's "tearing down System shares at logout would
    // destroy their entire purpose"). Partial is included: the helper's
    // disarm() accepts it too, so a stranded surviving half does not outlive
    // every logout untouched (plan §5.8).
    for (const Verify::OwnedUnit &unit : Verify::enumerateOwnedUnits(::getuid())) {
        if (unit.mode != UnitValue::CredentialMode::Session) {
            continue;
        }
        if (unit.state != Verify::Definition::Pair && unit.state != Verify::Definition::Partial) {
            continue;
        }
        const QString &mountPoint = unit.mountPoint;
        QString lockError;
        auto lock = Session::UserLock::acquire(&lockError);
        if (!lock) {
            out << "FAILED: " << mountPoint << " -> " << lockError << Qt::endl;
            ++failures;
            continue;
        }
        // Idempotent: disarm on an already-inactive pair is a defined no-op
        // in the helper (systemctl stop on an inactive unit succeeds).
        const Session::HelperResult result =
            Session::invokeHelperAction(QStringLiteral("disarm"), {{QStringLiteral("path"), mountPoint}});
        if (result.outcome != Session::HelperOutcome::ConfirmedSuccess) {
            out << "FAILED: " << mountPoint << " -> " << result.message << Qt::endl;
            ++failures;
        }
    }

    return failures;
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("nasmount-supervisor"));

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("Arms/disarms saved network shares for the graphical session."));
    parser.addHelpOption();
    QCommandLineOption startOption(QStringLiteral("start"), QStringLiteral("Arm every Reconnect=true share."));
    QCommandLineOption stopOption(QStringLiteral("stop"), QStringLiteral("Disarm every share owned by this user."));
    parser.addOption(startOption);
    parser.addOption(stopOption);
    parser.process(app);

    // Exit 0 regardless of the per-share failure count — see the file header.
    // The count is logged by the run functions themselves; propagating it as
    // an exit status would fail the unit and defeat RemainAfterExit=yes.
    if (parser.isSet(stopOption)) {
        const int failures = runStop();
        if (failures > 0) {
            QTextStream(stdout) << failures << " share(s) could not be disarmed; see above." << Qt::endl;
        }
        return 0;
    }
    if (parser.isSet(startOption)) {
        const int failures = runStart();
        if (failures > 0) {
            QTextStream(stdout) << failures << " share(s) could not be armed; see above." << Qt::endl;
        }
        return 0;
    }
    QTextStream(stderr) << "usage: nasmount-supervisor --start|--stop" << Qt::endl;
    return 2;
}
