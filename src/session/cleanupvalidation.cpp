/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "cleanupvalidation.h"

#include <QFile>
#include <QRegularExpression>
#include <QSet>

namespace Session
{

bool validateInstallManifest(const QString &manifestPath, QStringList *targets, QString *error)
{
    targets->clear();
    QFile file(manifestPath);
    if (!file.open(QIODevice::ReadOnly)) {
        *error = QStringLiteral("cannot read install manifest %1").arg(manifestPath);
        return false;
    }
    if (file.size() > 64 * 1024) {
        *error = QStringLiteral("install manifest is unexpectedly large");
        return false;
    }

    static const QRegularExpression allowed(QStringLiteral(
        "^/usr/(?:"
        "bin/nasmount-(?:dialog|supervisor|cleanup)|"
        "lib/[^/]+/libexec/kf6/kauth/nasmount-helper|"
        "lib/[^/]+/libexec/nasmount-boot|"
        "lib/[^/]+/qt6/plugins/plasma/kcms/systemsettings/kcm_nasmount\\.so|"
        "lib/systemd/user/nasmount-session\\.service|"
        "lib/systemd/system/nasmount-boot\\.service|"
        "share/applications/kcm_nasmount\\.desktop|"
        "share/polkit-1/actions/io\\.github\\.pakru\\.nasmount\\.policy|"
        "share/dbus-1/system\\.d/io\\.github\\.pakru\\.nasmount\\.conf|"
        "share/dbus-1/system-services/io\\.github\\.pakru\\.nasmount\\.service|"
        "share/kio/servicemenus/nasmount\\.desktop"
        ")$"));

    QSet<QString> seen;
    while (!file.atEnd()) {
        const QByteArray raw = file.readLine();
        if (raw.size() > 4096) {
            *error = QStringLiteral("install manifest contains an overlong line");
            return false;
        }
        const QString path = QString::fromUtf8(raw).trimmed();
        if (path.isEmpty()) {
            continue;
        }
        if (!allowed.match(path).hasMatch()) {
            *error = QStringLiteral("install manifest contains an unexpected path: %1").arg(path);
            return false;
        }
        if (seen.contains(path)) {
            *error = QStringLiteral("install manifest contains a duplicate path: %1").arg(path);
            return false;
        }
        seen.insert(path);
        targets->append(path);
    }
    static const QStringList requiredPatterns = {
        QStringLiteral("^/usr/bin/nasmount-dialog$"),
        QStringLiteral("^/usr/bin/nasmount-cleanup$"),
        QStringLiteral("^/usr/lib/[^/]+/libexec/kf6/kauth/nasmount-helper$"),
        QStringLiteral("^/usr/lib/[^/]+/libexec/nasmount-boot$"),
        QStringLiteral("^/usr/lib/[^/]+/qt6/plugins/plasma/kcms/systemsettings/kcm_nasmount\\.so$"),
        QStringLiteral("^/usr/lib/systemd/system/nasmount-boot\\.service$"),
        QStringLiteral("^/usr/share/applications/kcm_nasmount\\.desktop$"),
        QStringLiteral("^/usr/share/polkit-1/actions/io\\.github\\.pakru\\.nasmount\\.policy$"),
        QStringLiteral("^/usr/share/dbus-1/system\\.d/io\\.github\\.pakru\\.nasmount\\.conf$"),
        QStringLiteral("^/usr/share/dbus-1/system-services/io\\.github\\.pakru\\.nasmount\\.service$"),
        QStringLiteral("^/usr/share/kio/servicemenus/nasmount\\.desktop$"),
    };
    if (targets->size() != requiredPatterns.size()) {
        *error = QStringLiteral("install manifest is incomplete");
        return false;
    }
    for (const QString &pattern : requiredPatterns) {
        int matches = 0;
        const QRegularExpression required(pattern);
        for (const QString &target : *targets) {
            matches += required.match(target).hasMatch() ? 1 : 0;
        }
        if (matches != 1) {
            *error = QStringLiteral("install manifest is missing or duplicates an installed component");
            return false;
        }
    }
    return true;
}

} // namespace Session
