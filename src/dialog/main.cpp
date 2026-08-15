/*
 * nasmount-dialog — entry point.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Dolphin invokes this with the smb:// URL of the right-clicked folder, via the
 * service menu in servicemenu/nasmount.desktop.in. Arming saved shares at
 * sign-in is a separate binary, nasmount-supervisor — a GUI dialog has no
 * business running headless from a systemd unit.
 */

#include "mountdialog.h"

#include <KLocalizedString>
#include <KMessageBox>

#include <QApplication>
#include <QCommandLineParser>

#include <pwd.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    KLocalizedString::setApplicationDomain(QByteArrayLiteral("nasmount"));
    QApplication::setApplicationName(QStringLiteral("nasmount"));
    QApplication::setApplicationDisplayName(i18n("Mount as Network Drive"));
    QApplication::setDesktopFileName(QStringLiteral("nasmount"));

    QCommandLineParser parser;
    parser.setApplicationDescription(
        i18n("Mounts an SMB share on demand, with the password kept in KWallet."));
    parser.addHelpOption();
    parser.addPositionalArgument(QStringLiteral("url"), i18n("smb:// URL of the share to mount"));
    parser.process(app);

    const QStringList args = parser.positionalArguments();
    if (args.isEmpty()) {
        KMessageBox::error(nullptr, i18n("Usage: nasmount-dialog smb://host/share"));
        return 2;
    }

    QString unc;
    QString urlUser;
    QString error;
    if (!MountDialog::parseSmbUrl(args.first(), &unc, &urlUser, &error)) {
        KMessageBox::error(nullptr, error);
        return 2;
    }

    if (urlUser.isEmpty()) {
        if (const struct passwd *pw = ::getpwuid(::getuid())) {
            urlUser = QString::fromLocal8Bit(pw->pw_name);
        }
    }

    MountDialog dialog(unc, urlUser);
    dialog.show();
    return app.exec();
}
