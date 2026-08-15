/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "cleanupvalidation.h"

#include <QCoreApplication>
#include <QFile>
#include <QTemporaryDir>
#include <QTextStream>

namespace
{
int failures = 0;

void check(const QString &name, bool condition, const QString &detail = {})
{
    QTextStream out(stdout);
    out << (condition ? "PASS: " : "FAIL: ") << name;
    if (!detail.isEmpty()) {
        out << " (" << detail << ')';
    }
    out << Qt::endl;
    failures += condition ? 0 : 1;
}

bool writeManifest(const QString &path, const QString &content)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(content.toUtf8()) >= 0;
}
} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QTemporaryDir dir;
    check(QStringLiteral("temporary directory"), dir.isValid());
    const QString path = dir.filePath(QStringLiteral("install_manifest.txt"));

    const QString valid = QStringLiteral(
        "/usr/bin/nasmount-dialog\n"
        "/usr/bin/nasmount-supervisor\n"
        "/usr/bin/nasmount-cleanup\n"
        "/usr/lib/x86_64-linux-gnu/libexec/kf6/kauth/nasmount-helper\n"
        "/usr/lib/x86_64-linux-gnu/libexec/nasmount-boot\n"
        "/usr/lib/x86_64-linux-gnu/qt6/plugins/plasma/kcms/systemsettings/kcm_nasmount.so\n"
        "/usr/lib/systemd/user/nasmount-session.service\n"
        "/usr/lib/systemd/system/nasmount-boot.service\n"
        "/usr/share/applications/kcm_nasmount.desktop\n"
        "/usr/share/polkit-1/actions/io.github.pakru.nasmount.policy\n"
        "/usr/share/dbus-1/system.d/io.github.pakru.nasmount.conf\n"
        "/usr/share/dbus-1/system-services/io.github.pakru.nasmount.service\n"
        "/usr/share/kio/servicemenus/nasmount.desktop\n");
    check(QStringLiteral("write valid manifest"), writeManifest(path, valid));
    QStringList targets;
    QString error;
    check(QStringLiteral("complete allowlist accepted"),
          Session::validateInstallManifest(path, &targets, &error), error);
    check(QStringLiteral("all targets returned"), targets.size() == 13);

    check(QStringLiteral("write malicious manifest"), writeManifest(path, QStringLiteral("/etc/passwd\n")));
    error.clear();
    check(QStringLiteral("unexpected target rejected"),
          !Session::validateInstallManifest(path, &targets, &error));

    check(QStringLiteral("write duplicate manifest"),
          writeManifest(path, QStringLiteral("/usr/bin/nasmount-dialog\n/usr/bin/nasmount-dialog\n")));
    error.clear();
    check(QStringLiteral("duplicate target rejected"),
          !Session::validateInstallManifest(path, &targets, &error));

    check(QStringLiteral("write incomplete manifest"),
          writeManifest(path, QStringLiteral("/usr/bin/nasmount-cleanup\n")));
    error.clear();
    check(QStringLiteral("incomplete allowlisted manifest rejected"),
          !Session::validateInstallManifest(path, &targets, &error));

    return failures == 0 ? 0 : 1;
}
