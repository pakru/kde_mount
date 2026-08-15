/*
 * Table-driven tests for Session::classifyRow() (simplification-
 * implementation-plan.md §4 actions 5/6/8, §6.3).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * classifyRow() is pure: no filesystem, systemd, or Store access, so every
 * DisplayState and every actionability combination is reachable by
 * constructing a RowClassifyInput by hand, exactly like arming_test.cpp
 * does for evaluateArmPrecheck().
 */

#include "mountmodel.h"

#include <QCoreApplication>
#include <QTextStream>

static int passed = 0;
static int failed = 0;

static void check(const QString &label, bool condition, const QString &detail = QString())
{
    QTextStream out(stdout);
    out << (condition ? "  PASS  " : "  FAIL  ") << label;
    if (!detail.isEmpty()) {
        out << "   " << detail;
    }
    out << Qt::endl;
    condition ? ++passed : ++failed;
}

namespace
{

using Session::classifyRow;
using Session::DisplayState;
using Session::RowClassifyInput;

Verify::RuntimeSnapshot inactiveSnapshot()
{
    Verify::RuntimeSnapshot s;
    s.automount = Verify::AutomountState::Inactive;
    s.mount = Verify::MountState::Absent;
    return s;
}

Verify::RuntimeSnapshot armedTrustedSnapshot()
{
    Verify::RuntimeSnapshot s;
    s.automount = Verify::AutomountState::Active;
    s.mount = Verify::MountState::Absent;
    s.activationTrust = Verify::ActivationTrust::Trusted;
    return s;
}

Verify::RuntimeSnapshot armedUntrustedSnapshot()
{
    Verify::RuntimeSnapshot s;
    s.automount = Verify::AutomountState::Active;
    s.mount = Verify::MountState::Absent;
    s.activationTrust = Verify::ActivationTrust::Untrusted;
    return s;
}

Verify::RuntimeSnapshot mountedMatchSnapshot()
{
    Verify::RuntimeSnapshot s;
    s.automount = Verify::AutomountState::Active;
    s.mount = Verify::MountState::Present;
    s.verification = Verify::VerificationState::Match;
    s.activationTrust = Verify::ActivationTrust::Trusted;
    return s;
}

Verify::RuntimeSnapshot mountedMismatchSnapshot()
{
    Verify::RuntimeSnapshot s;
    s.automount = Verify::AutomountState::Inactive;
    s.mount = Verify::MountState::Present;
    s.verification = Verify::VerificationState::Mismatch;
    return s;
}

Verify::RuntimeSnapshot indeterminateMountSnapshot()
{
    Verify::RuntimeSnapshot s;
    s.automount = Verify::AutomountState::Inactive;
    s.mount = Verify::MountState::Indeterminate;
    return s;
}

Verify::RuntimeSnapshot indeterminateAutomountSnapshot()
{
    Verify::RuntimeSnapshot s;
    s.automount = Verify::AutomountState::Indeterminate;
    s.mount = Verify::MountState::Absent;
    return s;
}

RowClassifyInput pairInput(UnitValue::CredentialMode mode, UnitValue::AuthenticationKind auth,
                           const Verify::RuntimeSnapshot &rt, bool credApplicable = false, bool credHealthy = true)
{
    RowClassifyInput in;
    in.definitionState = QStringLiteral("pair");
    in.mode = mode;
    in.authentication = auth;
    in.runtime = rt;
    in.credentialApplicable = credApplicable;
    in.credentialHealthy = credHealthy;
    return in;
}

void checkActionability(const QString &label, const Session::RowClassification &c, bool canRemoveDefinition,
                        bool canRemoveLocalRecord, bool requiresAdministrator)
{
    check(label + QStringLiteral(": canRemoveDefinition"), c.canRemoveDefinition == canRemoveDefinition);
    check(label + QStringLiteral(": canRemoveLocalRecord"), c.canRemoveLocalRecord == canRemoveLocalRecord);
    check(label + QStringLiteral(": requiresAdministrator"), c.requiresAdministrator == requiresAdministrator);
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QTextStream out(stdout);

    out << "=== exact-ID Store/definition drift comparison ===" << Qt::endl;
    {
        using Session::storeDefinitionDrift;
        check(QStringLiteral("matching Store and definition are not drift"),
              !storeDefinitionDrift(QStringLiteral("//host/share"), QStringLiteral("/mnt/share"),
                                    UnitValue::CredentialMode::Session, /*storeSaysGuest=*/false,
                                    QStringLiteral("//host/share"), QStringLiteral("/mnt/share"),
                                    UnitValue::CredentialMode::Session,
                                    UnitValue::AuthenticationKind::Credentials));
        check(QStringLiteral("equivalent UNC with trailing slash is normalised"),
              !storeDefinitionDrift(QStringLiteral("//host/share/"), QStringLiteral("/mnt/share"),
                                    UnitValue::CredentialMode::Session, /*storeSaysGuest=*/false,
                                    QStringLiteral("//host/share"), QStringLiteral("/mnt/share"),
                                    UnitValue::CredentialMode::Session,
                                    UnitValue::AuthenticationKind::Credentials));
        check(QStringLiteral("mount-point mismatch is drift"),
              storeDefinitionDrift(QStringLiteral("//host/share"), QStringLiteral("/mnt/other"),
                                   UnitValue::CredentialMode::Session, /*storeSaysGuest=*/false,
                                   QStringLiteral("//host/share"), QStringLiteral("/mnt/share"),
                                   UnitValue::CredentialMode::Session,
                                   UnitValue::AuthenticationKind::Credentials));
        check(QStringLiteral("UNC mismatch is drift"),
              storeDefinitionDrift(QStringLiteral("//host/other"), QStringLiteral("/mnt/share"),
                                   UnitValue::CredentialMode::Session, /*storeSaysGuest=*/false,
                                   QStringLiteral("//host/share"), QStringLiteral("/mnt/share"),
                                   UnitValue::CredentialMode::Session,
                                   UnitValue::AuthenticationKind::Credentials));
        check(QStringLiteral("mode mismatch is drift"),
              storeDefinitionDrift(QStringLiteral("//host/share"), QStringLiteral("/mnt/share"),
                                   UnitValue::CredentialMode::System, /*storeSaysGuest=*/false,
                                   QStringLiteral("//host/share"), QStringLiteral("/mnt/share"),
                                   UnitValue::CredentialMode::Session,
                                   UnitValue::AuthenticationKind::Credentials));
        check(QStringLiteral("authentication mismatch is drift"),
              storeDefinitionDrift(QStringLiteral("//host/share"), QStringLiteral("/mnt/share"),
                                   UnitValue::CredentialMode::Session, /*storeSaysGuest=*/true,
                                   QStringLiteral("//host/share"), QStringLiteral("/mnt/share"),
                                   UnitValue::CredentialMode::Session,
                                   UnitValue::AuthenticationKind::Credentials));
        check(QStringLiteral("automount-only Partial defers unavailable UNC comparison"),
              !storeDefinitionDrift(QStringLiteral("//host/share"), QStringLiteral("/mnt/share"),
                                    UnitValue::CredentialMode::Session, /*storeSaysGuest=*/false,
                                    QString(), QStringLiteral("/mnt/share"),
                                    UnitValue::CredentialMode::Session,
                                    UnitValue::AuthenticationKind::Credentials));
    }

    out << "=== Store-only rows (definitionState == none) ===" << Qt::endl;
    {
        RowClassifyInput in;
        in.definitionState = QStringLiteral("none");
        in.hasStoreRecord = true;
        const auto c = classifyRow(in);
        check(QStringLiteral("hasStoreRecord, well-formed -> Broken"), c.state == DisplayState::Broken);
        checkActionability(QStringLiteral("Store-only, well-formed"), c, false, true, false);
    }
    {
        RowClassifyInput in;
        in.definitionState = QStringLiteral("none");
        in.hasStoreRecord = true;
        in.storeCorrupt = true;
        const auto c = classifyRow(in);
        check(QStringLiteral("hasStoreRecord, corrupt -> Broken"), c.state == DisplayState::Broken);
        check(QStringLiteral("corrupt detail mentions missing data"), c.detail.contains(QStringLiteral("missing")),
              c.detail);
        checkActionability(QStringLiteral("Store-only, corrupt"), c, false, true, false);
    }

    out << "=== Tampered / NotOurs ===" << Qt::endl;
    {
        RowClassifyInput in;
        in.definitionState = QStringLiteral("tampered");
        const auto c = classifyRow(in);
        check(QStringLiteral("Tampered, no Store record -> Broken"), c.state == DisplayState::Broken);
        checkActionability(QStringLiteral("Tampered, no Store record"), c, false, false, true);
    }
    {
        RowClassifyInput in;
        in.definitionState = QStringLiteral("notOurs");
        in.hasStoreRecord = true;
        const auto c = classifyRow(in);
        check(QStringLiteral("NotOurs, with Store record -> Broken"), c.state == DisplayState::Broken);
        checkActionability(QStringLiteral("NotOurs, with Store record"), c, false, true, true);
    }

    out << "=== Store drift / corruption on an owned Pair ===" << Qt::endl;
    {
        RowClassifyInput in = pairInput(UnitValue::CredentialMode::Session, UnitValue::AuthenticationKind::Credentials,
                                        mountedMatchSnapshot());
        in.hasStoreRecord = true;
        in.drift = true;
        const auto c = classifyRow(in);
        check(QStringLiteral("drift always wins over a healthy runtime -> Broken"), c.state == DisplayState::Broken);
        checkActionability(QStringLiteral("drift"), c, false, true, false);
    }
    {
        RowClassifyInput in = pairInput(UnitValue::CredentialMode::Session, UnitValue::AuthenticationKind::Credentials,
                                        inactiveSnapshot());
        in.hasStoreRecord = true;
        in.storeCorrupt = true;
        const auto c = classifyRow(in);
        check(QStringLiteral("corrupt Store data on a Pair -> Broken"), c.state == DisplayState::Broken);
        checkActionability(QStringLiteral("Pair + storeCorrupt"), c, false, true, false);
    }

    out << "=== Partial ===" << Qt::endl;
    {
        RowClassifyInput in;
        in.definitionState = QStringLiteral("partial");
        in.runtime = inactiveSnapshot();
        const auto c = classifyRow(in);
        check(QStringLiteral("clean Partial -> Broken, removable, never repaired"), c.state == DisplayState::Broken);
        checkActionability(QStringLiteral("clean Partial"), c, true, false, false);
    }
    {
        RowClassifyInput in;
        in.definitionState = QStringLiteral("partial");
        in.runtime = mountedMismatchSnapshot();
        const auto c = classifyRow(in);
        check(QStringLiteral("Partial with a non-correlating live mount -> Busy, takes precedence"),
              c.state == DisplayState::Busy);
        checkActionability(QStringLiteral("Busy Partial"), c, false, false, false);
    }
    {
        RowClassifyInput in;
        in.definitionState = QStringLiteral("partial");
        in.runtime = indeterminateMountSnapshot();
        const auto c = classifyRow(in);
        check(QStringLiteral("Partial with indeterminate runtime -> Busy"), c.state == DisplayState::Busy);
        checkActionability(QStringLiteral("Indeterminate Partial"), c, false, false, false);
    }
    {
        RowClassifyInput in;
        in.definitionState = QStringLiteral("partial");
        in.runtime = armedUntrustedSnapshot();
        const auto c = classifyRow(in);
        check(QStringLiteral("Partial with an untrusted active trigger -> Broken, administrator"),
              c.state == DisplayState::Broken);
        checkActionability(QStringLiteral("untrusted-active Partial"), c, false, false, true);
    }

    out << "=== Pair: runtime-only classification (guest, or no fresh credential data) ===" << Qt::endl;
    {
        const auto c = classifyRow(pairInput(UnitValue::CredentialMode::Session,
                                             UnitValue::AuthenticationKind::Guest, inactiveSnapshot()));
        check(QStringLiteral("guest, inactive -> Inactive"), c.state == DisplayState::Inactive);
        checkActionability(QStringLiteral("guest inactive"), c, true, false, false);
    }
    {
        const auto c = classifyRow(pairInput(UnitValue::CredentialMode::Session,
                                             UnitValue::AuthenticationKind::Guest, armedTrustedSnapshot()));
        check(QStringLiteral("guest, armed -> Armed"), c.state == DisplayState::Armed);
    }
    {
        const auto c = classifyRow(pairInput(UnitValue::CredentialMode::Session,
                                             UnitValue::AuthenticationKind::Guest, mountedMatchSnapshot()));
        check(QStringLiteral("guest, mounted -> Mounted"), c.state == DisplayState::Mounted);
    }
    {
        // Guest rows never have credentialApplicable set, but even if a
        // caller mistakenly passed unhealthy data, rule 4 (design §4.4)
        // makes it inert for a guest row.
        const auto c = classifyRow(pairInput(UnitValue::CredentialMode::System, UnitValue::AuthenticationKind::Guest,
                                             armedTrustedSnapshot(), /*credApplicable=*/false,
                                             /*credHealthy=*/false));
        check(QStringLiteral("guest ignores credential health even if somehow flagged"),
              c.state == DisplayState::Armed);
    }

    out << "=== Pair: the mode-dependent credential rule (design/simplification §4.4) ===" << Qt::endl;
    {
        const auto c = classifyRow(pairInput(UnitValue::CredentialMode::Session,
                                             UnitValue::AuthenticationKind::Credentials, inactiveSnapshot(),
                                             /*credApplicable=*/true, /*credHealthy=*/false));
        check(QStringLiteral("inactive Session + missing credential -> Inactive (absence expected)"),
              c.state == DisplayState::Inactive);
    }
    {
        const auto c = classifyRow(pairInput(UnitValue::CredentialMode::Session,
                                             UnitValue::AuthenticationKind::Credentials, armedTrustedSnapshot(),
                                             /*credApplicable=*/true, /*credHealthy=*/false));
        check(QStringLiteral("active Session + missing credential -> MissingCredentials"),
              c.state == DisplayState::MissingCredentials);
    }
    {
        const auto c = classifyRow(pairInput(UnitValue::CredentialMode::Session,
                                             UnitValue::AuthenticationKind::Credentials, mountedMatchSnapshot(),
                                             /*credApplicable=*/true, /*credHealthy=*/false));
        check(QStringLiteral("mounted Session + missing credential -> MissingCredentials"),
              c.state == DisplayState::MissingCredentials);
    }
    {
        const auto c = classifyRow(pairInput(UnitValue::CredentialMode::System,
                                             UnitValue::AuthenticationKind::Credentials, inactiveSnapshot(),
                                             /*credApplicable=*/true, /*credHealthy=*/false));
        check(QStringLiteral("inactive System + missing credential -> MissingCredentials (persistent, always expected)"),
              c.state == DisplayState::MissingCredentials);
    }
    {
        const auto c = classifyRow(pairInput(UnitValue::CredentialMode::System,
                                             UnitValue::AuthenticationKind::Credentials, armedTrustedSnapshot(),
                                             /*credApplicable=*/true, /*credHealthy=*/false));
        check(QStringLiteral("active System + missing credential -> MissingCredentials"),
              c.state == DisplayState::MissingCredentials);
    }
    {
        const auto c = classifyRow(pairInput(UnitValue::CredentialMode::System,
                                             UnitValue::AuthenticationKind::Credentials, inactiveSnapshot(),
                                             /*credApplicable=*/true, /*credHealthy=*/true));
        check(QStringLiteral("inactive System + healthy credential -> Inactive"), c.state == DisplayState::Inactive);
    }
    {
        // No fresh inventory data this refresh (credentialApplicable still
        // false) must not be mistaken for "missing" -- the row keeps its
        // runtime-only classification.
        const auto c = classifyRow(pairInput(UnitValue::CredentialMode::System,
                                             UnitValue::AuthenticationKind::Credentials, inactiveSnapshot(),
                                             /*credApplicable=*/false, /*credHealthy=*/false));
        check(QStringLiteral("no fresh credential data yet -> runtime-only Inactive, not guessed as missing"),
              c.state == DisplayState::Inactive);
    }

    out << "=== Pair: runtime safety always wins over credential health (rule 1) ===" << Qt::endl;
    {
        const auto c = classifyRow(pairInput(UnitValue::CredentialMode::System,
                                             UnitValue::AuthenticationKind::Credentials, armedUntrustedSnapshot(),
                                             /*credApplicable=*/true, /*credHealthy=*/false));
        check(QStringLiteral("untrusted active + missing credential -> Broken, not MissingCredentials"),
              c.state == DisplayState::Broken);
        checkActionability(QStringLiteral("untrusted-active + missing credential"), c, false, false, true);
    }
    {
        const auto c = classifyRow(pairInput(UnitValue::CredentialMode::System,
                                             UnitValue::AuthenticationKind::Credentials, indeterminateAutomountSnapshot(),
                                             /*credApplicable=*/true, /*credHealthy=*/false));
        check(QStringLiteral("indeterminate + missing credential -> Busy, not MissingCredentials"),
              c.state == DisplayState::Busy);
    }
    {
        const auto c = classifyRow(pairInput(UnitValue::CredentialMode::System,
                                             UnitValue::AuthenticationKind::Credentials, mountedMismatchSnapshot(),
                                             /*credApplicable=*/true, /*credHealthy=*/false));
        check(QStringLiteral("non-correlating live mount + missing credential -> Busy, not MissingCredentials"),
              c.state == DisplayState::Busy);
    }

    out << "=== Pair: healthy/clean runtime states and their actionability ===" << Qt::endl;
    {
        const auto c = classifyRow(pairInput(UnitValue::CredentialMode::Session,
                                             UnitValue::AuthenticationKind::Credentials, inactiveSnapshot(),
                                             /*credApplicable=*/true, /*credHealthy=*/true));
        check(QStringLiteral("healthy inactive Session -> Inactive"), c.state == DisplayState::Inactive);
        checkActionability(QStringLiteral("healthy Inactive"), c, true, false, false);
    }
    {
        const auto c = classifyRow(pairInput(UnitValue::CredentialMode::Session,
                                             UnitValue::AuthenticationKind::Credentials, armedTrustedSnapshot(),
                                             /*credApplicable=*/true, /*credHealthy=*/true));
        checkActionability(QStringLiteral("healthy Armed"), c, true, false, false);
    }
    {
        const auto c = classifyRow(pairInput(UnitValue::CredentialMode::Session,
                                             UnitValue::AuthenticationKind::Credentials, mountedMatchSnapshot(),
                                             /*credApplicable=*/true, /*credHealthy=*/true));
        checkActionability(QStringLiteral("healthy Mounted"), c, true, false, false);
    }
    {
        const auto c = classifyRow(pairInput(UnitValue::CredentialMode::System,
                                             UnitValue::AuthenticationKind::Credentials, armedTrustedSnapshot(),
                                             /*credApplicable=*/true, /*credHealthy=*/false));
        checkActionability(QStringLiteral("MissingCredentials (active)"), c, true, false, false);
    }

    out << "=== Foreign is never produced by classifyRow() itself ===" << Qt::endl;
    {
        // Foreign rows are assembled directly in computeRefresh() from
        // unclaimed mountinfo entries -- classifyRow() only ever sees an
        // owned definition or a Store record, documented here so the
        // omission is not mistaken for a gap.
        check(QStringLiteral("(no case: Foreign is not reachable through classifyRow())"), true);
    }

    out << Qt::endl << passed << " passed, " << failed << " failed" << Qt::endl;
    return failed == 0 ? 0 : 1;
}
