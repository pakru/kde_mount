/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "userlock.h"

#include <QDir>
#include <QStandardPaths>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

namespace Session
{

UserLock::UserLock(UserLock &&other) noexcept : fd_(other.fd_)
{
    other.fd_ = -1;
}

UserLock &UserLock::operator=(UserLock &&other) noexcept
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

UserLock::~UserLock()
{
    if (fd_ >= 0) {
        ::flock(fd_, LOCK_UN);
        ::close(fd_);
    }
}

std::unique_ptr<UserLock> UserLock::acquire(QString *error)
{
    const QString runtimeDir = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    if (runtimeDir.isEmpty()) {
        *error = QStringLiteral("XDG_RUNTIME_DIR is not set");
        return nullptr;
    }
    QDir().mkpath(runtimeDir);
    const QString path = runtimeDir + QStringLiteral("/nasmount.lock");

    std::unique_ptr<UserLock> lock(new UserLock);
    lock->fd_ = ::open(path.toLocal8Bit().constData(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (lock->fd_ < 0) {
        *error = QStringLiteral("cannot open %1: %2").arg(path, QString::fromLocal8Bit(strerror(errno)));
        return nullptr;
    }
    if (::flock(lock->fd_, LOCK_EX) != 0) {
        *error = QStringLiteral("cannot lock %1: %2").arg(path, QString::fromLocal8Bit(strerror(errno)));
        return nullptr;
    }
    return lock;
}

} // namespace Session
