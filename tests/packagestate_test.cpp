/*
 * Rootless tests for the native-package removal state classifier.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Every fixture lives below a temporary root. This proves fail-closed
 * classification without requiring sudo or inspecting the workstation's real
 * package state.
 */

#include "packagestate.h"
#include "unitvalue.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTextStream>

#include <unistd.h>

namespace
{
int passed = 0;
int failed = 0;

void check(const QString &label, bool condition, const QString &detail = {})
{
    QTextStream out(stdout);
    out << (condition ? "  PASS  " : "  FAIL  ") << label;
    if (!detail.isEmpty()) {
        out << "   " << detail;
    }
    out << Qt::endl;
    condition ? ++passed : ++failed;
}

bool writeFile(const QString &path, const QByteArray &content)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(content) == content.size();
}
} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QTemporaryDir root;
    check(QStringLiteral("temporary root created"), root.isValid());
    QDir().mkpath(root.filePath(QStringLiteral("etc/systemd/system")));

    auto classify = [&root]() { return PackageState::classify(root.path()); };
    check(QStringLiteral("empty root accepted"),
          classify().classification == PackageState::Classification::Empty);

    const QString unrelated = root.filePath(QStringLiteral("etc/systemd/system/unrelated.mount"));
    check(QStringLiteral("unrelated unit written"), writeFile(unrelated, "[Mount]\nWhere=/mnt/other\n"));
    check(QStringLiteral("unrelated unit ignored"),
          classify().classification == PackageState::Classification::Empty);

    UnitValue::Marker marker;
    marker.ownerUid = ::getuid();
    marker.ownerGid = ::getgid();
    marker.id = QStringLiteral("0123456789abcdef0123456789abcdef");
    const QString managed = root.filePath(QStringLiteral("etc/systemd/system/managed.mount"));
    check(QStringLiteral("managed marker written"),
          writeFile(managed, UnitValue::markerComment(marker).toUtf8()));
    check(QStringLiteral("valid marker blocks removal"),
          classify().classification == PackageState::Classification::Managed);

    QFile::remove(managed);
    check(QStringLiteral("malformed marker written"),
          writeFile(managed, "# X-Nasmount-Managed=1\n"));
    check(QStringLiteral("malformed marker is unsafe"),
          classify().classification == PackageState::Classification::Unsafe);
    QFile::remove(managed);

    QDir().mkpath(root.filePath(QStringLiteral("etc/nasmount")));
    check(QStringLiteral("credential root blocks removal"),
          classify().classification == PackageState::Classification::Managed);
    QDir(root.filePath(QStringLiteral("etc/nasmount"))).removeRecursively();

    QDir().mkpath(root.filePath(QStringLiteral("run/nasmount")));
    check(QStringLiteral("private runtime root blocks removal"),
          classify().classification == PackageState::Classification::Managed);
    QDir(root.filePath(QStringLiteral("run/nasmount"))).removeRecursively();

    QDir().mkpath(root.filePath(QStringLiteral("run/nasmount-ids")));
    check(QStringLiteral("public runtime root blocks removal"),
          classify().classification == PackageState::Classification::Managed);
    QDir(root.filePath(QStringLiteral("run/nasmount-ids"))).removeRecursively();

    const QString danglingState = root.filePath(QStringLiteral("etc/nasmount"));
    check(QStringLiteral("dangling state symlink planted"),
          QFile::link(root.filePath(QStringLiteral("missing-target")), danglingState));
    check(QStringLiteral("dangling state symlink blocks removal"),
          classify().classification == PackageState::Classification::Managed);
    QFile::remove(danglingState);

    const QString unreadable = root.filePath(QStringLiteral("etc/systemd/system/unreadable.mount"));
    check(QStringLiteral("unreadable candidate written"), writeFile(unreadable, "[Mount]\n"));
    QFile unreadableFile(unreadable);
    check(QStringLiteral("candidate made unreadable"),
          unreadableFile.setPermissions(QFileDevice::Permissions{}));
    const PackageState::Result unreadableState = classify();
    check(QStringLiteral("read failure is indeterminate"),
          unreadableState.classification == PackageState::Classification::Indeterminate,
          unreadableState.detail);
    unreadableFile.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);

    check(QStringLiteral("relative test root rejected"),
          PackageState::classify(QStringLiteral("relative")).classification
              == PackageState::Classification::Indeterminate);

    QTextStream(stdout) << passed << " passed, " << failed << " failed\n";
    return failed == 0 ? 0 : 1;
}
