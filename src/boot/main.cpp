/*
 * nasmount-boot — the root systemd oneshot that arms System-mode shares at
 * boot (design §6.3, plan §4.2).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Started directly by systemd as root (nasmount-boot.service); there is no
 * D-Bus/KAuth caller to authorize here, since this was always going to run
 * as root the moment the unit is enabled. Installs autofs triggers only —
 * it never mounts anything itself, so an unreachable NAS costs nothing here
 * (design §6.3's "the coordinator only arms triggers"). Exits non-zero only
 * if the root lock cannot be acquired; a single share's own failure is
 * logged and skipped, never fatal to the rest (design §6.3's "per-share
 * validation/start failures do not fail the whole oneshot").
 */

#include "arming.h"
#include "nasmountversion.h"
#include "rootlock.h"
#include "unitspec.h"
#include "unitvalue.h"
#include "verify.h"

#include <QCoreApplication>
#include <QCommandLineParser>
#include <QTextStream>

#include <pwd.h>

namespace
{

/** journald captures a systemd service's stderr directly; no separate
 *  logging dependency is needed for a bounded per-share diagnostic. */
void logLine(const QString &message)
{
    QTextStream(stderr) << "nasmount-boot: " << message << Qt::endl;
}

/**
 * One System pair's boot-time arm attempt. Every failure is logged and
 * swallowed here -- never fatal to the rest of the run (design §6.3.6).
 */
void armOneShare(const Verify::OwnedUnit &unit)
{
    const struct passwd *pw = ::getpwuid(unit.ownerUid);
    if (!pw) {
        logLine(QStringLiteral("%1: owner uid %2 no longer resolves to an account -- leaving unarmed")
                    .arg(unit.mountPoint)
                    .arg(unit.ownerUid));
        return;
    }
    const QString homeDir = QString::fromLocal8Bit(pw->pw_dir);

    // Re-run the full parent authorization, not just trust the marker's
    // recorded Where= (plan §4.2.3's "resolve the recorded uid to a live
    // account" -- for its home directory here -- "while retaining marker
    // uid/gid for ownership checks", which openMountpointNoCreate() below
    // does via unit.ownerUid/ownerGid, never pw->pw_uid/pw_gid).
    UnitSpec::MountpointPlan plan;
    QString planError;
    if (!UnitSpec::validateMountpoint(unit.mountPoint, homeDir, &plan, &planError)) {
        logLine(QStringLiteral("%1: %2 -- leaving unarmed").arg(unit.mountPoint, planError));
        return;
    }

    UnitValue::UnitPaths paths;
    QString pathsError;
    if (!UnitValue::unitPathsFor(plan.path, &paths, &pathsError) || paths.unitName != unit.unitName) {
        logLine(QStringLiteral("%1: unit name does not re-derive from its own Where= -- leaving unarmed")
                    .arg(unit.mountPoint));
        return;
    }

    // The full structural/ownership check (design §6.3.2's "re-running
    // every parent §4.1 ownership and §6.1 structural check on both
    // halves"), not only the lighter pairing check enumerateManagedUnits()
    // already did.
    const Verify::DefinitionCheck def = Verify::inspectDefinition(paths, unit.ownerUid, plan.path);
    if (def.state != Verify::Definition::Pair) {
        logLine(QStringLiteral("%1: %2 -- leaving unarmed")
                    .arg(plan.path, def.detail.isEmpty() ? QStringLiteral("failed re-validation") : def.detail));
        return;
    }

    Root::Arming::ArmShareRequest req;
    req.ownerUid = unit.ownerUid;
    req.ownerGid = unit.ownerGid;
    req.shareId = unit.id;
    req.authentication = unit.authentication;
    req.plan = plan;
    req.unitName = unit.unitName;
    req.what = def.what;
    req.pathPolicy = Root::Arming::PathPolicy::BootNoCreate;

    const Root::Arming::ArmShareResult result = Root::Arming::armShare(req);
    if (result.outcome == Root::Arming::ArmShareOutcome::NeedsAttention) {
        logLine(QStringLiteral("%1: %2").arg(plan.path, result.error));
    }
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("nasmount-boot"));
    QCoreApplication::setApplicationVersion(QString::fromLatin1(Nasmount::Version));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Arm nasmount shares at boot"));
    parser.addHelpOption();
    parser.addVersionOption();
    parser.process(app);

    QString lockError;
    auto lock = Root::RootLock::acquire(&lockError);
    if (!lock) {
        logLine(QStringLiteral("cannot acquire the root lock: %1").arg(lockError));
        return 1;
    }

    // Enumerate every validated System pair, across every owner, and arm
    // each safe inactive one.
    const QList<Verify::OwnedUnit> units = Verify::enumerateManagedUnits();
    int attempted = 0;
    int skipped = 0;
    for (const Verify::OwnedUnit &unit : units) {
        if (unit.state != Verify::Definition::Pair) {
            ++skipped;
            const QString label = unit.mountPoint.isEmpty() ? unit.unitName : unit.mountPoint;
            logLine(QStringLiteral("%1: %2 -- leaving unarmed")
                        .arg(label, unit.detail.isEmpty() ? QStringLiteral("not a complete, agreeing pair")
                                                          : unit.detail));
            continue;
        }
        armOneShare(unit);
        ++attempted;
    }

    logLine(QStringLiteral("processed %1 System share(s), %2 skipped (not a valid pair)")
                .arg(attempted)
                .arg(skipped));
    return 0;
}
