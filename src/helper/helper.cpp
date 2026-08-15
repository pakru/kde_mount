/*
 * nasmount KAuth helper — the privileged half.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Runs as root, activated on the system bus by KAuth. Everything in the
 * argument map is untrusted: the dialog and KCM are unprivileged and a
 * hostile process can call these actions directly, so every check happens
 * here, freshly, under the root lock, regardless of what a caller claims.
 *
 * Caller identity comes from KAuth::HelperSupport::callerUid(), never from
 * the argument map, so the mount-point allowlist, the uid=/gid= options and
 * the owner-uid marker are scoped to who really called.
 *
 * This file is deliberately thin (plan §2.1.6): caller validation, typed
 * argument decoding, root-lock acquisition, dispatch into nasmount-root, and
 * structured reply conversion. Every privileged filesystem/systemd mutation
 * lives in nasmount-root (durablefs, credentialstore, runtimefiles,
 * systemdops, arming, operations) — nothing here writes a file, starts/stops
 * a unit, or touches a credential directly any more.
 */

#include "arming.h"
#include "inventory.h"
#include "operations.h"
#include "rootlock.h"
#include "systemdops.h"
#include "unitspec.h"
#include "unitvalue.h"
#include "verify.h"

#include <KAuth/ActionReply>
#include <KAuth/HelperSupport>

#include <QCoreApplication>
#include <QVariantMap>

#include <pwd.h>

using namespace KAuth;

namespace
{

/**
 * Control-character and per-field size checks shared by every call site that
 * accepts credential fields (design §8.1: "cap every credential field ...
 * apply the same validation in arm and both define paths"). The same limits
 * are enforced again inside Root::CredentialStore::write() — this is the
 * fast, early rejection; that is the one no call path can skip.
 */
bool validateCredentialFields(const QString &username, const QString &domain, const QString &password,
                              QString *error)
{
    if (UnitSpec::hasControlChars(username) || UnitSpec::hasControlChars(domain)
        || UnitSpec::hasControlChars(password)) {
        *error = QStringLiteral("credential fields contain control characters");
        return false;
    }
    if (username.toUtf8().size() > UnitSpec::MaxCredentialFieldBytes
        || domain.toUtf8().size() > UnitSpec::MaxCredentialFieldBytes
        || password.toUtf8().size() > UnitSpec::MaxCredentialFieldBytes) {
        *error = QStringLiteral("a credential field exceeds %1 bytes").arg(UnitSpec::MaxCredentialFieldBytes);
        return false;
    }
    return true;
}

/**
 * Design §8.1: "an empty username means guest ... domain and password must
 * also be empty or the helper rejects the request instead of silently
 * discarding them." Shared by every action that accepts credential fields
 * alongside a username (arm, definesystem).
 */
bool guestFieldsConsistent(const QString &username, const QString &domain, const QString &password)
{
    return !username.isEmpty() || (domain.isEmpty() && password.isEmpty());
}

ActionReply fail(const QString &message)
{
    ActionReply reply = ActionReply::HelperErrorReply();
    reply.setErrorDescription(message);
    return reply;
}

ActionReply ok(const QString &message = QString(), const QVariantMap &extra = {})
{
    ActionReply reply = ActionReply::SuccessReply();
    if (!message.isEmpty()) {
        reply.addData(QStringLiteral("message"), message);
    }
    for (auto it = extra.constBegin(); it != extra.constEnd(); ++it) {
        reply.addData(it.key(), it.value());
    }
    return reply;
}

struct CallerInfo {
    uid_t uid = 0;
    gid_t gid = 0;
    QString home;
};

bool resolveCaller(CallerInfo *info, QString *error)
{
    const int uid = HelperSupport::callerUid();
    if (uid < 0) {
        *error = QStringLiteral("cannot determine the calling user");
        return false;
    }
    const struct passwd *pw = ::getpwuid(static_cast<uid_t>(uid));
    if (!pw) {
        *error = QStringLiteral("uid %1 does not correspond to a real user").arg(uid);
        return false;
    }
    info->uid = pw->pw_uid;
    info->gid = pw->pw_gid;
    info->home = QString::fromLocal8Bit(pw->pw_dir);
    return true;
}

/** Resolves caller + path + unit paths together, the first step of every action. */
bool prepare(const QVariantMap &args, CallerInfo *caller, UnitSpec::MountpointPlan *plan,
            UnitValue::UnitPaths *paths, QString *error)
{
    if (!resolveCaller(caller, error)) {
        return false;
    }
    if (!UnitSpec::validateMountpoint(args.value(QStringLiteral("path")).toString(), caller->home, plan, error)) {
        return false;
    }
    return UnitValue::unitPathsFor(plan->path, paths, error);
}

} // namespace

class NasMountHelper : public QObject
{
    Q_OBJECT

public Q_SLOTS:
    ActionReply define(const QVariantMap &args);
    ActionReply definesystem(const QVariantMap &args);
    ActionReply undefine(const QVariantMap &args);
    ActionReply undefinesystem(const QVariantMap &args);
    ActionReply arm(const QVariantMap &args);
    ActionReply disarm(const QVariantMap &args);
    ActionReply mountnow(const QVariantMap &args);
    ActionReply unmountnow(const QVariantMap &args);
    ActionReply inventory(const QVariantMap &args);
    ActionReply purge(const QVariantMap &args);
};

namespace
{

/**
 * Shared by `define` and `definesystem` (plan §3.1): `mode` is hard-coded by
 * the caller from which action was invoked, never read from `args` (design
 * §7.1). A fresh create only — an existing Partial half is never repaired;
 * it must be removed first (simplification-implementation-plan.md §4.1/§4.3).
 */
ActionReply doDefine(const QVariantMap &args, UnitValue::CredentialMode mode)
{
    QString error;
    auto lock = Root::RootLock::acquire(&error);
    if (!lock) {
        return fail(error);
    }

    CallerInfo caller;
    UnitSpec::MountpointPlan plan;
    UnitValue::UnitPaths paths;
    if (!prepare(args, &caller, &plan, &paths, &error)) {
        return fail(error);
    }

    QString unc;
    if (!UnitSpec::validateUnc(args.value(QStringLiteral("unc")).toString(), &unc, &error)) {
        return fail(error);
    }
    const QString username = args.value(QStringLiteral("username")).toString();
    const QString domain = args.value(QStringLiteral("domain")).toString();
    const QString password = args.value(QStringLiteral("password")).toString();
    if (mode == UnitValue::CredentialMode::System) {
        // Only definesystem takes domain/password at define time (design
        // §8.1) -- Session's credential is written later, by arm().
        if (!validateCredentialFields(username, domain, password, &error)) {
            return fail(error);
        }
        if (!guestFieldsConsistent(username, domain, password)) {
            return fail(
                QStringLiteral("a share with no username is guest, and cannot also have a password or domain"));
        }
    } else if (UnitSpec::hasControlChars(username)) {
        return fail(QStringLiteral("username contains control characters"));
    }

    const auto def = Verify::inspectDefinition(paths, caller.uid, plan.path);
    if (def.state == Verify::Definition::Partial) {
        return fail(QStringLiteral("cannot define %1: a broken definition already exists here — remove it first")
                        .arg(plan.path));
    }
    if (def.state != Verify::Definition::None) {
        return fail(QStringLiteral("cannot define %1: %2")
                        .arg(plan.path,
                             def.detail.isEmpty() ? QStringLiteral("a definition already exists") : def.detail));
    }

    // The full descriptor walk (not only at arm): without it, define() would
    // happily write a unit whose Where= is a non-empty directory or an
    // ancestor of another mount's Where=. That unit is `static` and never
    // armed by define alone, but systemd still loads it, and
    // systemd.mount(5) makes any mount unit automatically a dependency of
    // another mount unit nested beneath it in the filesystem — so a
    // mistakenly-placed, never-armed definition can still break unrelated,
    // already-working mounts the moment daemon-reload runs.
    const int mpFd = UnitSpec::openMountpointNoFollow(plan, caller.uid, caller.gid, &error);
    if (mpFd < 0) {
        return fail(error);
    }
    ::close(mpFd);

    Root::Operations::DefineInput input;
    input.ownerUid = caller.uid;
    input.ownerGid = caller.gid;
    input.unc = unc;
    input.mountPoint = plan.path;
    input.unitName = paths.unitName;
    input.username = username;
    input.mode = mode;
    if (mode == UnitValue::CredentialMode::System) {
        input.domain = domain;
        input.password = password;
        input.mountPlan = plan; // needed for the immediate-arm path walk (design §6.3a)
    }

    const Root::Operations::DefineOutput result = Root::Operations::define(input);
    if (!result.ok) {
        return fail(result.error);
    }
    // The client indexes its Store record by this id (plan §1.6.2, §5.1) --
    // it is never allowed to invent its own. `activated` reflects a real
    // System immediate-arm result (design §6.3a); always false for Session,
    // where activation is a separate, later `arm` call.
    return ok(QStringLiteral("Defined %1 -> %2").arg(unc, plan.path),
             {{QStringLiteral("id"), result.shareId}, {QStringLiteral("activated"), result.activated}});
}

} // namespace

ActionReply NasMountHelper::define(const QVariantMap &args)
{
    return doDefine(args, UnitValue::CredentialMode::Session);
}

ActionReply NasMountHelper::definesystem(const QVariantMap &args)
{
    return doDefine(args, UnitValue::CredentialMode::System);
}

namespace
{

/** Design §7.1: "existing mode must be Session" for undefine,
 *  "require System" for undefinesystem -- derived from the
 *  validated on-disk marker (`def.mode`), never from an argument. */
QString modeMismatchError(const QString &verb, const QString &path, UnitValue::CredentialMode required)
{
    return QStringLiteral("cannot %1 %2: it is not a %3-mode definition; use the matching action")
        .arg(verb, path, required == UnitValue::CredentialMode::System ? QStringLiteral("System") : QStringLiteral("Session"));
}

ActionReply doUndefine(const QVariantMap &args, UnitValue::CredentialMode mode)
{
    QString error;
    auto lock = Root::RootLock::acquire(&error);
    if (!lock) {
        return fail(error);
    }

    CallerInfo caller;
    UnitSpec::MountpointPlan plan;
    UnitValue::UnitPaths paths;
    if (!prepare(args, &caller, &plan, &paths, &error)) {
        return fail(error);
    }

    const auto def = Verify::inspectDefinition(paths, caller.uid, plan.path);
    if (def.state != Verify::Definition::Pair && def.state != Verify::Definition::Partial) {
        return fail(QStringLiteral("cannot undefine %1: %2")
                        .arg(plan.path,
                             def.detail.isEmpty() ? QStringLiteral("no definition owned by you exists") : def.detail));
    }
    if (def.mode != mode) {
        return fail(modeMismatchError(QStringLiteral("undefine"), plan.path, mode));
    }

    Root::Operations::RemovalInput input;
    input.ownerUid = def.ownerUid;
    input.ownerGid = def.ownerGid;
    input.shareId = def.id;
    input.mode = def.mode;
    input.authentication = def.authentication;
    input.mountPoint = plan.path;
    input.unitName = paths.unitName;
    input.what = def.what;

    const Root::Operations::RemovalOutput result = Root::Operations::remove(input);
    if (!result.ok) {
        return fail(result.error);
    }
    return ok(QStringLiteral("Undefined %1").arg(plan.path));
}

} // namespace

ActionReply NasMountHelper::undefine(const QVariantMap &args)
{
    return doUndefine(args, UnitValue::CredentialMode::Session);
}

ActionReply NasMountHelper::undefinesystem(const QVariantMap &args)
{
    return doUndefine(args, UnitValue::CredentialMode::System);
}

ActionReply NasMountHelper::arm(const QVariantMap &args)
{
    QString error;
    auto lock = Root::RootLock::acquire(&error);
    if (!lock) {
        return fail(error);
    }

    CallerInfo caller;
    UnitSpec::MountpointPlan plan;
    UnitValue::UnitPaths paths;
    if (!prepare(args, &caller, &plan, &paths, &error)) {
        return fail(error);
    }

    const QString username = args.value(QStringLiteral("username")).toString();
    const QString domain = args.value(QStringLiteral("domain")).toString();
    const QString password = args.value(QStringLiteral("password")).toString();
    if (!validateCredentialFields(username, domain, password, &error)) {
        return fail(error);
    }

    const auto def = Verify::inspectDefinition(paths, caller.uid, plan.path);
    if (def.state != Verify::Definition::Pair) {
        return fail(QStringLiteral("cannot arm %1: %2")
                        .arg(plan.path, def.detail.isEmpty() ? QStringLiteral("no owned definition exists") : def.detail));
    }
    if (def.mode != UnitValue::CredentialMode::Session) {
        // System shares are armed by nasmount-boot, never by this passwordless
        // action (design §7.1/§4.4's "authentication costs nothing at sign-in
        // ... does not cover System").
        return fail(QStringLiteral("cannot arm %1: it is a System-mode share, armed automatically at boot")
                        .arg(plan.path));
    }
    if (def.authentication == UnitValue::AuthenticationKind::Credentials && username.isEmpty()) {
        return fail(QStringLiteral("this share requires a username"));
    }
    if (!guestFieldsConsistent(username, domain, password)) {
        return fail(
            QStringLiteral("a share with no username is guest, and cannot also have a password or domain"));
    }
    const Verify::RuntimeSnapshot snapshot = Verify::inspectRuntime(paths.unitName, plan.path, def.what);
    if (snapshot.automount != Verify::AutomountState::Inactive || snapshot.mount != Verify::MountState::Absent) {
        return fail(QStringLiteral("cannot arm %1: already armed or mounted").arg(plan.path));
    }

    // The path was validated at define time, possibly long ago — re-run the
    // full descriptor walk now, while it is still an ordinary directory,
    // before starting anything.
    const int mpFd = UnitSpec::openMountpointNoFollow(plan, caller.uid, caller.gid, &error);
    if (mpFd < 0) {
        return fail(error);
    }
    ::close(mpFd);

    Root::Arming::ArmRequest req;
    req.ownerUid = def.ownerUid;
    req.ownerGid = def.ownerGid;
    req.shareId = def.id;
    req.mode = def.mode;
    req.authentication = def.authentication;
    req.mountPoint = plan.path;
    req.unitName = paths.unitName;
    req.what = def.what;
    req.username = username;
    req.domain = domain;
    req.password = password;

    if (!Root::Arming::arm(req, &error)) {
        return fail(error);
    }
    return ok(QStringLiteral("%1 armed").arg(plan.path));
}

ActionReply NasMountHelper::disarm(const QVariantMap &args)
{
    QString error;
    auto lock = Root::RootLock::acquire(&error);
    if (!lock) {
        return fail(error);
    }

    CallerInfo caller;
    UnitSpec::MountpointPlan plan;
    UnitValue::UnitPaths paths;
    if (!prepare(args, &caller, &plan, &paths, &error)) {
        return fail(error);
    }

    const auto def = Verify::inspectDefinition(paths, caller.uid, plan.path);
    // Partial accepted too (plan §5.8/design §12.2): the session supervisor's
    // teardown sweep must be able to stop a stranded surviving half, and
    // safeStop()'s correlation gate underneath already handles either half
    // correctly on its own -- refusing Partial here would only leave such a
    // share strandable at every logout.
    if (def.state != Verify::Definition::Pair && def.state != Verify::Definition::Partial) {
        return fail(QStringLiteral("cannot disarm %1: %2")
                        .arg(plan.path, def.detail.isEmpty() ? QStringLiteral("no owned definition exists") : def.detail));
    }
    if (def.mode != UnitValue::CredentialMode::Session) {
        return fail(QStringLiteral("cannot disarm %1: it is a System-mode share, managed by nasmount-boot")
                        .arg(plan.path));
    }

    Root::Arming::DisarmRequest req;
    req.ownerUid = def.ownerUid;
    req.ownerGid = def.ownerGid;
    req.shareId = def.id;
    req.mode = def.mode;
    req.authentication = def.authentication;
    req.mountPoint = plan.path;
    req.unitName = paths.unitName;
    req.what = def.what;

    if (!Root::Arming::disarm(req, &error)) {
        return fail(error);
    }
    return ok(QStringLiteral("%1 disarmed").arg(plan.path));
}

ActionReply NasMountHelper::mountnow(const QVariantMap &args)
{
    // mount-now/unmount-now never change credential lifetime or a
    // definition (plan §2.6.7) -- they only trigger/release the CIFS mount
    // behind an already-armed, already-trusted automount.
    QString error;
    auto lock = Root::RootLock::acquire(&error);
    if (!lock) {
        return fail(error);
    }

    CallerInfo caller;
    UnitSpec::MountpointPlan plan;
    UnitValue::UnitPaths paths;
    if (!prepare(args, &caller, &plan, &paths, &error)) {
        return fail(error);
    }

    const auto def = Verify::inspectDefinition(paths, caller.uid, plan.path);
    if (def.state != Verify::Definition::Pair) {
        return fail(QStringLiteral("cannot mount %1: %2")
                        .arg(plan.path, def.detail.isEmpty() ? QStringLiteral("no owned definition exists") : def.detail));
    }
    if (def.mode != UnitValue::CredentialMode::Session) {
        return fail(QStringLiteral("cannot mount %1: it is a System-mode share").arg(plan.path));
    }
    const Verify::RuntimeSnapshot snapshot = Verify::inspectRuntime(paths.unitName, plan.path, def.what);
    if (snapshot.automount != Verify::AutomountState::Active || snapshot.mount != Verify::MountState::Absent) {
        return fail(QStringLiteral("cannot mount %1: automount is not armed, or it is already mounted").arg(plan.path));
    }
    // Never open() the path here: doing so would trigger the very mount this
    // is validating. The recorded instance id is the only safe check
    // available at this point.
    if (snapshot.activationTrust != Verify::ActivationTrust::Trusted) {
        return fail(QStringLiteral("cannot mount %1: the armed automount is not the instance this helper created")
                        .arg(plan.path));
    }

    if (!Root::SystemdOps::start(paths.unitName + QStringLiteral(".mount"), &error)) {
        return fail(error);
    }
    return ok(QStringLiteral("%1 mounted").arg(plan.path));
}

ActionReply NasMountHelper::unmountnow(const QVariantMap &args)
{
    QString error;
    auto lock = Root::RootLock::acquire(&error);
    if (!lock) {
        return fail(error);
    }

    CallerInfo caller;
    UnitSpec::MountpointPlan plan;
    UnitValue::UnitPaths paths;
    if (!prepare(args, &caller, &plan, &paths, &error)) {
        return fail(error);
    }

    const auto def = Verify::inspectDefinition(paths, caller.uid, plan.path);
    if (def.state != Verify::Definition::Pair) {
        return fail(QStringLiteral("cannot unmount %1: %2")
                        .arg(plan.path, def.detail.isEmpty() ? QStringLiteral("no owned definition exists") : def.detail));
    }
    if (def.mode != UnitValue::CredentialMode::Session) {
        return fail(QStringLiteral("cannot unmount %1: it is a System-mode share").arg(plan.path));
    }
    const Verify::RuntimeSnapshot snapshot = Verify::inspectRuntime(paths.unitName, plan.path, def.what);
    if (snapshot.mount != Verify::MountState::Present || snapshot.verification != Verify::VerificationState::Match) {
        return fail(QStringLiteral("cannot unmount %1: not mounted, or does not correlate with this definition")
                        .arg(plan.path));
    }

    if (!Root::SystemdOps::stop(paths.unitName + QStringLiteral(".mount"), &error)) {
        return fail(QStringLiteral("unmount failed: %1").arg(error));
    }
    return ok(QStringLiteral("%1 unmounted").arg(plan.path));
}

ActionReply NasMountHelper::inventory(const QVariantMap &args)
{
    // Read-only, caller-scoped (design §7.1/§3.3.1): no argument is ever
    // read from `args` -- every record is derived from what is actually on
    // disk for the resolved caller uid, never from anything the caller
    // claims.
    Q_UNUSED(args);
    QString error;
    auto lock = Root::RootLock::acquire(&error);
    if (!lock) {
        return fail(error);
    }

    CallerInfo caller;
    if (!resolveCaller(&caller, &error)) {
        return fail(error);
    }

    QString buildError;
    const QList<Root::Inventory::ShareRecord> records = Root::Inventory::buildFor(caller.uid, &buildError);
    if (!buildError.isEmpty()) {
        return fail(buildError);
    }
    return ok(QStringLiteral("%1 share(s)").arg(records.size()),
             {{QStringLiteral("shares"), Root::Inventory::toJson(records)}});
}

ActionReply NasMountHelper::purge(const QVariantMap &args)
{
    // The action accepts no paths, ids, or mode supplied by the caller. Its
    // scope is derived exclusively from the authenticated caller and the
    // verified root-owned artifacts on disk.
    Q_UNUSED(args);
    QString error;
    auto lock = Root::RootLock::acquire(&error);
    if (!lock) {
        return fail(error);
    }

    CallerInfo caller;
    if (!resolveCaller(&caller, &error)) {
        return fail(error);
    }
    const Root::Operations::PurgeOutput result = Root::Operations::purge(caller.uid);
    if (!result.ok) {
        return fail(result.error);
    }
    return ok(QStringLiteral("purged %1 managed share(s)").arg(result.removedShares));
}

KAUTH_HELPER_MAIN("io.github.pakru.nasmount", NasMountHelper)

#include "helper.moc"
