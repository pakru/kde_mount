/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "rootlock.h"
#include "durablefs.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

namespace Root
{

namespace
{
const QLatin1String LockFileName("helper.lock");
}

RootLock::RootLock(RootLock &&other) noexcept : fd_(other.fd_)
{
    other.fd_ = -1;
}

RootLock &RootLock::operator=(RootLock &&other) noexcept
{
    if (this != &other) {
        if (fd_ >= 0) {
            ::flock(fd_, LOCK_UN);
            ::close(fd_);
        }
        fd_ = other.fd_;
        other.fd_ = -1;
    }
    return *this;
}

RootLock::~RootLock()
{
    if (fd_ >= 0) {
        ::flock(fd_, LOCK_UN);
        ::close(fd_);
    }
}

std::unique_ptr<RootLock> RootLock::acquire(QString *error)
{
    QString err;
    const int runFd = DurableFs::openSystemRoot(QStringLiteral("/run"), &err);
    if (runFd < 0) {
        *error = err;
        return nullptr;
    }
    const int dirFd = DurableFs::createAndVerifyDir(runFd, QStringLiteral("nasmount"), &err);
    ::close(runFd);
    if (dirFd < 0) {
        *error = err;
        return nullptr;
    }

    const QByteArray raw(LockFileName.data(), LockFileName.size());
    const int fd = ::openat(dirFd, raw.constData(), O_CREAT | O_RDWR | O_NOFOLLOW | O_CLOEXEC, 0600);
    ::close(dirFd);
    if (fd < 0) {
        *error =
            QStringLiteral("cannot open lock file: %1").arg(QString::fromLocal8Bit(strerror(errno)));
        return nullptr;
    }
    QString verifyError;
    if (!DurableFs::verifyDescriptor(fd, DurableFs::FileKind::Regular, 0, 0600, &verifyError)) {
        *error = QStringLiteral("refusing to use lock file: %1").arg(verifyError);
        ::close(fd);
        return nullptr;
    }
    if (::flock(fd, LOCK_EX) != 0) {
        *error = QStringLiteral("cannot acquire lock: %1").arg(QString::fromLocal8Bit(strerror(errno)));
        ::close(fd);
        return nullptr;
    }

    std::unique_ptr<RootLock> lock(new RootLock);
    lock->fd_ = fd;
    return lock;
}

} // namespace Root
