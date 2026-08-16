/*
 * Tests for Dialog::SmbUrl (the service menu's smb:// parsing).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This parsing was untestable while it lived as a static member of the
 * QWidgets dialog; extracting it for the shared-QML front end made it
 * reachable, so pin the behaviour its comments describe -- especially the
 * two non-obvious cases: Dolphin substitutes %u with the URL *as displayed*,
 * so literal spaces must parse, while a malformed %-escape must be refused
 * rather than guessed at (Qt's repair mode would otherwise silently resolve
 * "Media%20Library" to a different share name than intended).
 *
 * None of this is a security boundary -- the KAuth helper re-validates every
 * field -- but a wrong UNC here means mounting the wrong share.
 */

#include "smburl.h"

#include <QCoreApplication>
#include <QDir>
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

/** Accept-path helper: parses and reports the resulting UNC/user. */
void expectParsed(const QString &label, const QString &raw, const QString &expectedUnc,
                  const QString &expectedUser)
{
    QString unc;
    QString user;
    QString error;
    const bool ok = Dialog::SmbUrl::parse(raw, &unc, &user, &error);
    check(label, ok && unc == expectedUnc && user == expectedUser,
          ok ? QStringLiteral("unc=%1 user=%2").arg(unc, user) : error);
}

void expectRejected(const QString &label, const QString &raw)
{
    QString unc;
    QString user;
    QString error;
    const bool ok = Dialog::SmbUrl::parse(raw, &unc, &user, &error);
    check(label, !ok && !error.isEmpty(), ok ? QStringLiteral("accepted as %1").arg(unc) : error);
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QTextStream out(stdout);

    out << "=== parse: accepted forms ===" << Qt::endl;
    expectParsed(QStringLiteral("plain host/share"), QStringLiteral("smb://10.0.0.10/DATA"),
                 QStringLiteral("//10.0.0.10/DATA"), QString());
    expectParsed(QStringLiteral("nested subdirectories are kept"),
                 QStringLiteral("smb://10.0.0.10/DATA/Pavel/Docs/Torrents"),
                 QStringLiteral("//10.0.0.10/DATA/Pavel/Docs/Torrents"), QString());
    expectParsed(QStringLiteral("user in the URL is recovered"),
                 QStringLiteral("smb://pa_kru@nas.local/DATA"), QStringLiteral("//nas.local/DATA"),
                 QStringLiteral("pa_kru"));
    expectParsed(QStringLiteral("percent-encoded space decodes"),
                 QStringLiteral("smb://nas.local/Media%20Library"),
                 QStringLiteral("//nas.local/Media Library"), QString());
    // Dolphin passes the URL as displayed; a literal space must still work.
    expectParsed(QStringLiteral("literal space (Dolphin %u substitution) parses"),
                 QStringLiteral("smb://nas.local/Media Library"),
                 QStringLiteral("//nas.local/Media Library"), QString());
    expectParsed(QStringLiteral("trailing slash is trimmed"), QStringLiteral("smb://nas.local/DATA/"),
                 QStringLiteral("//nas.local/DATA"), QString());
    expectParsed(QStringLiteral("encoded percent stays a literal percent"),
                 QStringLiteral("smb://nas.local/100%25"), QStringLiteral("//nas.local/100%"), QString());

    out << "=== parse: rejections ===" << Qt::endl;
    expectRejected(QStringLiteral("non-smb scheme"), QStringLiteral("http://nas.local/DATA"));
    expectRejected(QStringLiteral("server-only URL, no share"), QStringLiteral("smb://nas.local"));
    expectRejected(QStringLiteral("server-only URL with slash"), QStringLiteral("smb://nas.local/"));
    expectRejected(QStringLiteral("no host"), QStringLiteral("smb:///DATA"));
    expectRejected(QStringLiteral("port is not supported"), QStringLiteral("smb://nas.local:445/DATA"));
    expectRejected(QStringLiteral("password component is refused"),
                   QStringLiteral("smb://user:secret@nas.local/DATA"));
    expectRejected(QStringLiteral("empty-but-present password component is refused"),
                   QStringLiteral("smb://user:@nas.local/DATA"));
    expectRejected(QStringLiteral("query is refused"), QStringLiteral("smb://nas.local/DATA?x=1"));
    expectRejected(QStringLiteral("fragment is refused"), QStringLiteral("smb://nas.local/DATA#frag"));
    expectRejected(QStringLiteral("IPv6 host is refused"), QStringLiteral("smb://[fe80::1]/DATA"));
    // A lone '%' would put Qt into repair mode for every escape in the URL,
    // silently changing which share is meant -- refuse instead of guessing.
    expectRejected(QStringLiteral("malformed %-escape is refused, never repaired"),
                   QStringLiteral("smb://nas.local/Media%20Library/100%"));
    expectRejected(QStringLiteral("empty input"), QString());

    out << "=== suggestMountpoint ===" << Qt::endl;
    {
        const QString home = QDir::homePath();
        check(QStringLiteral("uses the share's own leaf name under $HOME"),
              Dialog::SmbUrl::suggestMountpoint(QStringLiteral("//nas.local/DATA/Docs/Torrents"))
                  == home + QStringLiteral("/Torrents"),
              Dialog::SmbUrl::suggestMountpoint(QStringLiteral("//nas.local/DATA/Docs/Torrents")));
        check(QStringLiteral("trailing slash does not produce an empty leaf"),
              Dialog::SmbUrl::suggestMountpoint(QStringLiteral("//nas.local/DATA/"))
                  == home + QStringLiteral("/DATA"),
              Dialog::SmbUrl::suggestMountpoint(QStringLiteral("//nas.local/DATA/")));
        check(QStringLiteral("degenerate input falls back to a usable name"),
              Dialog::SmbUrl::suggestMountpoint(QStringLiteral("//")) == home + QStringLiteral("/Share"),
              Dialog::SmbUrl::suggestMountpoint(QStringLiteral("//")));
    }

    out << Qt::endl << passed << " passed, " << failed << " failed" << Qt::endl;
    return failed == 0 ? 0 : 1;
}
