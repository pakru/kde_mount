/*
 * nasmount-cleanup — authenticated uninstall coordinator.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This remains an unprivileged command-line client: it validates the exact
 * install manifest before asking the helper to purge privileged state, then
 * removes the caller's configuration only after that purge is confirmed.
 */

#include "cleanupvalidation.h"
#include "helperinvoke.h"
#include "nasmountversion.h"
#include "store.h"
#include "userlock.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QTextStream>

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("nasmount-cleanup"));
    QCoreApplication::setApplicationVersion(QString::fromLatin1(Nasmount::Version));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Authenticated nasmount uninstall cleanup"));
    parser.addHelpOption();
    parser.addVersionOption();
    QCommandLineOption manifestOption(QStringLiteral("manifest"), QStringLiteral("CMake install manifest"),
                                      QStringLiteral("path"));
    parser.addOption(manifestOption);
    parser.process(app);

    QTextStream err(stderr);
    if (!parser.isSet(manifestOption)) {
        err << "ERROR: --manifest is required\n";
        return 2;
    }

    QString error;
    QStringList targets;
    if (!Session::validateInstallManifest(parser.value(manifestOption), &targets, &error)) {
        err << "ERROR: " << error << '\n';
        return 1;
    }

    auto lock = Session::UserLock::acquire(&error);
    if (!lock) {
        err << "ERROR: " << error << '\n';
        return 1;
    }

    const Session::HelperResult purge = Session::invokeHelperAction(QStringLiteral("purge"), {});
    if (purge.outcome != Session::HelperOutcome::ConfirmedSuccess) {
        err << "ERROR: privileged purge was not confirmed: " << purge.message << '\n';
        return 1;
    }
    if (!Store::purgeApplicationData(&error)) {
        err << "ERROR: privileged data was purged, but user data cleanup failed: " << error << '\n';
        return 1;
    }

    QTextStream(stdout) << purge.message << "; removed local configuration\n";
    return 0;
}
