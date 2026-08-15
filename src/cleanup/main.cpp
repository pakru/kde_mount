/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "cleanupvalidation.h"
#include "helperinvoke.h"
#include "store.h"
#include "userlock.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QTextStream>

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("nasmount-cleanup"));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Authenticated nasmount uninstall cleanup"));
    parser.addHelpOption();
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

    QTextStream(stdout) << purge.message << "; removed KWallet folder and configuration\n";
    return 0;
}
