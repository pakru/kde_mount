/*
 * unitvalue — the one encoder every value written into a generated unit file
 * goes through, plus the unit-naming and marker-v2 helpers built on top of it.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Rejecting only a trailing backslash (the previous, transient-era check) is
 * not enough once values are written into real unit *files* rather than
 * passed as argv to systemd-mount: a share name or mount point that contains
 * '%' is corrupted by specifier expansion unless doubled, so this exists to
 * make that mandatory rather than optional at each call site.
 */

#pragma once

#include <QString>

#include <sys/types.h>

namespace UnitValue
{

/**
 * Validates and encodes one value for a `Key=value` unit-file assignment.
 *
 * Verified empirically against systemd 255 (`systemctl --user show` on a
 * loaded unit, not just `systemd-analyze verify`, which only checks syntax):
 * outside of an explicitly *quoted* token, systemd's unit-file parser does
 * not interpret backslashes or embedded quote characters at all. A value is
 * parsed as a quoted token only when its first non-whitespace character is a
 * quote mark, in which case C-style escapes and early termination at the
 * matching quote apply (systemd.syntax(7)). So this does not escape a quoting
 * syntax — the fixed option list and root-generated share id never need
 * one — it guarantees the value can never be *mistaken* for one, and doubles
 * '%' because specifier expansion is confirmed to apply to What=, Where= and
 * Options= alike (a bare Where=…%h-… was expanded to the caller's home
 * directory).
 *
 * Rejects: control characters, leading/trailing whitespace, a trailing
 * backslash (line-continuation), and a value whose first non-whitespace
 * character is a quote mark. None of the paths this tool ever generates
 * legitimately start with a quote; refusing is simpler and safer than adding
 * a quoting mode nothing needs.
 */
bool encodeUnitValue(const QString &value, QString *encoded, QString *error);

/**
 * The two unit paths a share's pair lives at, and the bare escaped name used
 * to derive both.
 */
struct UnitPaths {
    QString unitName;         ///< systemd-escape -p output, no suffix
    QString mountUnitPath;    ///< /etc/systemd/system/<unitName>.mount
    QString automountUnitPath; ///< /etc/systemd/system/<unitName>.automount
};

/**
 * Derives a share's UnitPaths from its canonical mount point via
 * `systemd-escape --path`. Fails if the escape cannot be produced; the
 * resulting names are what both generation and validation key off, so this
 * is the single definition of "which files this share lives in".
 */
bool unitPathsFor(const QString &mountPoint, UnitPaths *paths, QString *error);

/**
 * Whether a share carries a credential artifact at all. Guest shares use the
 * fixed `guest` mount option and have no credential file. Design §4, §8.1.
 */
enum class AuthenticationKind { Credentials, Guest };

/** True iff `id` is exactly 32 lowercase hex characters (design §5.1). */
bool isValidShareId(const QString &id);

/**
 * The marker-v2 fields, both written to and required from every unit half.
 * Design §6.1. There is no partial/optional representation: a value only
 * exists here once the complete marker has parsed successfully.
 */
struct Marker {
    uid_t ownerUid = 0;
    gid_t ownerGid = 0;
    QString id; ///< 32 lowercase hex characters
    AuthenticationKind authentication = AuthenticationKind::Credentials;

    bool operator==(const Marker &other) const
    {
        return ownerUid == other.ownerUid && ownerGid == other.ownerGid && id == other.id
            && authentication == other.authentication;
    }
    bool operator!=(const Marker &other) const { return !(*this == other); }
};

/**
 * The marker-v2 comment block written into both generated unit files
 * (design §6.1):
 *
 *   # X-Nasmount-Managed=1
 *   # X-Nasmount-Owner-Uid=<uid>
 *   # X-Nasmount-Owner-Gid=<gid>
 *   # X-Nasmount-Id=<32 lowercase hex>
 *   # X-Nasmount-Mode=system
 *   # X-Nasmount-Authentication=credentials|guest
 *
 * `marker.id` must already satisfy isValidShareId(); this is asserted, not
 * re-validated, because every caller either just generated the id or already
 * proved it via parseMarker().
 */
QString markerComment(const Marker &marker);

/**
 * Returns true if unit file content contains anything in the managed marker
 * namespace (any `# X-Nasmount-...` line), even if incomplete or malformed.
 * False means an ordinary, non-nasmount unit — "not ours". This is distinct
 * from parseMarker() succeeding, which additionally proves the marker is a
 * complete, well-formed marker-v2 block: callers must treat "hasMarker() true
 * but parseMarker() false" as Tampered, never as absent.
 */
bool hasMarker(const QString &unitFileContent);

/**
 * Parses the marker-v2 block out of unit file content.
 *
 * Succeeds only when all six fields above are present exactly once, each
 * holds a syntactically valid value, and no other `# X-Nasmount-...` line
 * exists anywhere in the content. Any deviation — a duplicate field, a
 * missing field, an unknown field in the managed marker namespace (including
 * the old `Credential-Id` spelling, which is never parsed), an invalid
 * integer, an invalid id, or an unrecognised mode/authentication value —
 * fails closed: this returns false and every output is left unmodified.
 * There is exactly one success path.
 */
bool parseMarker(const QString &unitFileContent, Marker *marker, QString *error);

} // namespace UnitValue
