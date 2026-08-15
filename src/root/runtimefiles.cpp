/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "runtimefiles.h"
#include "durablefs.h"

namespace Root::RuntimeFiles
{

namespace
{

/** Opens (creating as needed) /run/nasmount/<subdir>, root-owned 0700. */
int openRuntimeSubdir(const QString &subdir, QString *error)
{
    const int runFd = DurableFs::openSystemRoot(QStringLiteral("/run"), error);
    if (runFd < 0) {
        return -1;
    }
    const int nasmountFd = DurableFs::createAndVerifyDir(runFd, QStringLiteral("nasmount"), error);
    ::close(runFd);
    if (nasmountFd < 0) {
        return -1;
    }
    const int subFd = DurableFs::createAndVerifyDir(nasmountFd, subdir, error);
    ::close(nasmountFd);
    return subFd;
}

} // namespace

bool writeAutomountId(const QString &unitName, uint64_t id, QString *error)
{
    const int dirFd = openRuntimeSubdir(QStringLiteral("automount-ids"), error);
    if (dirFd < 0) {
        return false;
    }
    const QByteArray content = QByteArray::number(static_cast<qulonglong>(id));
    const bool ok =
        DurableFs::durableReplace(dirFd, unitName, content, DurableFs::ArtifactKind::SensitiveFile, error);
    ::close(dirFd);
    return ok;
}

bool removeAutomountId(const QString &unitName, QString *error)
{
    const int dirFd = openRuntimeSubdir(QStringLiteral("automount-ids"), error);
    if (dirFd < 0) {
        return false;
    }
    const bool ok = DurableFs::durableUnlink(dirFd, unitName, /*allowMissing=*/true, error);
    ::close(dirFd);
    return ok;
}

} // namespace Root::RuntimeFiles
