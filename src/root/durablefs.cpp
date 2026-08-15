/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "durablefs.h"

#include <QRandomGenerator64>

#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace Root::DurableFs
{

namespace
{

QString errnoText()
{
    return QString::fromLocal8Bit(strerror(errno));
}

/** Single path components only: durable operations always take one name at
 *  a time against an already-opened, already-verified directory fd, never a
 *  nested path a caller could use to escape it. */
bool isSingleComponent(const QString &name)
{
    return !name.isEmpty() && name != QStringLiteral(".") && name != QStringLiteral("..")
        && !name.contains(QLatin1Char('/'));
}

} // namespace

mode_t modeFor(ArtifactKind kind)
{
    switch (kind) {
    case ArtifactKind::Directory:
        return 0700;
    case ArtifactKind::UnitFile:
        return 0644;
    case ArtifactKind::SensitiveFile:
        return 0600;
    }
    return 0600;
}

bool verifyDescriptor(int fd, FileKind expectedType, uid_t expectedUid, mode_t expectedMode, QString *error)
{
    struct stat st {};
    if (::fstat(fd, &st) != 0) {
        *error = QStringLiteral("cannot stat descriptor: %1").arg(errnoText());
        return false;
    }
    const bool typeOk = (expectedType == FileKind::Directory) ? S_ISDIR(st.st_mode) : S_ISREG(st.st_mode);
    if (!typeOk) {
        *error = QStringLiteral("not a %1").arg(expectedType == FileKind::Directory ? QStringLiteral("directory")
                                                                                     : QStringLiteral("regular file"));
        return false;
    }
    if (st.st_uid != expectedUid) {
        *error = QStringLiteral("owned by uid %1, expected %2").arg(st.st_uid).arg(expectedUid);
        return false;
    }
    if ((st.st_mode & 07777) != expectedMode) {
        *error = QStringLiteral("mode is %1, expected %2")
                     .arg(st.st_mode & 07777, 0, 8)
                     .arg(expectedMode, 0, 8);
        return false;
    }
    return true;
}

int openSystemRoot(const QString &absolutePath, QString *error)
{
    const QByteArray raw = absolutePath.toLocal8Bit();
    const int fd = ::open(raw.constData(), O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
        *error = QStringLiteral("cannot open %1: %2").arg(absolutePath, errnoText());
        return -1;
    }
    struct stat st {};
    if (::fstat(fd, &st) != 0 || !S_ISDIR(st.st_mode) || st.st_uid != 0 || (st.st_mode & (S_IWGRP | S_IWOTH))) {
        *error = QStringLiteral("%1 is not a safe root-owned, non-group/other-writable directory").arg(absolutePath);
        ::close(fd);
        return -1;
    }
    return fd;
}

int openVerifiedDir(int parentFd, const QString &name, QString *error)
{
    if (!isSingleComponent(name)) {
        *error = QStringLiteral("invalid directory component: %1").arg(name);
        return -1;
    }
    const QByteArray raw = name.toLocal8Bit();
    const int fd = ::openat(parentFd, raw.constData(), O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
        *error = (errno == ENOENT) ? QString() : QStringLiteral("cannot open %1: %2").arg(name, errnoText());
        return -1;
    }
    QString verifyError;
    if (!verifyDescriptor(fd, FileKind::Directory, 0, modeFor(ArtifactKind::Directory), &verifyError)) {
        *error = QStringLiteral("%1: %2").arg(name, verifyError);
        ::close(fd);
        return -1;
    }
    return fd;
}

int createAndVerifyDir(int parentFd, const QString &name, QString *error)
{
    if (!isSingleComponent(name)) {
        *error = QStringLiteral("invalid directory component: %1").arg(name);
        return -1;
    }
    const QByteArray raw = name.toLocal8Bit();
    bool created = false;
    if (::mkdirat(parentFd, raw.constData(), modeFor(ArtifactKind::Directory)) == 0) {
        created = true;
    } else if (errno != EEXIST) {
        *error = QStringLiteral("cannot create %1: %2").arg(name, errnoText());
        return -1;
    }
    // A newly created directory is not durable until the directory entry
    // naming it is durable too. fsyncing files inside the new directory
    // does not persist its entry in the parent after power loss.
    if (created && ::fsync(parentFd) != 0) {
        *error = QStringLiteral("cannot fsync parent directory after creating %1: %2").arg(name, errnoText());
        return -1;
    }
    QString openError;
    const int fd = openVerifiedDir(parentFd, name, &openError);
    if (fd < 0) {
        *error = openError.isEmpty() ? QStringLiteral("cannot open just-created %1").arg(name) : openError;
        return -1;
    }
    return fd;
}

bool readFileBounded(int dirFd, const QString &name, qint64 maxBytes, ArtifactKind kind, QByteArray *content,
                     QString *error)
{
    if (!isSingleComponent(name)) {
        *error = QStringLiteral("invalid file component: %1").arg(name);
        return false;
    }
    const QByteArray raw = name.toLocal8Bit();
    const int fd = ::openat(dirFd, raw.constData(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
        *error = (errno == ENOENT) ? QString() : QStringLiteral("cannot open %1: %2").arg(name, errnoText());
        return false;
    }
    QString verifyError;
    if (!verifyDescriptor(fd, FileKind::Regular, 0, modeFor(kind), &verifyError)) {
        *error = QStringLiteral("%1: %2").arg(name, verifyError);
        ::close(fd);
        return false;
    }
    struct stat st {};
    if (::fstat(fd, &st) != 0) {
        *error = QStringLiteral("cannot stat %1: %2").arg(name, errnoText());
        ::close(fd);
        return false;
    }
    if (st.st_size > maxBytes) {
        *error = QStringLiteral("%1 exceeds the %2-byte limit").arg(name).arg(maxBytes);
        ::close(fd);
        return false;
    }
    QByteArray buffer;
    buffer.resize(static_cast<int>(st.st_size));
    qint64 total = 0;
    while (total < buffer.size()) {
        const ssize_t n = ::read(fd, buffer.data() + total, static_cast<size_t>(buffer.size() - total));
        if (n < 0) {
            *error = QStringLiteral("cannot read %1: %2").arg(name, errnoText());
            ::close(fd);
            return false;
        }
        if (n == 0) {
            break; // file shrank concurrently; bounded by what was actually read
        }
        total += n;
    }
    buffer.resize(static_cast<int>(total));
    ::close(fd);
    *content = buffer;
    return true;
}

bool durableReplace(int dirFd, const QString &name, const QByteArray &content, ArtifactKind kind, QString *error)
{
    if (!isSingleComponent(name)) {
        *error = QStringLiteral("invalid file component: %1").arg(name);
        return false;
    }

    QString tmpName;
    int fd = -1;
    for (int attempt = 0; attempt < 8 && fd < 0; ++attempt) {
        tmpName = QStringLiteral(".nasmount-tmp-%1")
                      .arg(QRandomGenerator64::global()->generate64(), 16, 16, QLatin1Char('0'));
        const QByteArray raw = tmpName.toLocal8Bit();
        fd = ::openat(dirFd, raw.constData(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (fd < 0 && errno != EEXIST) {
            *error = QStringLiteral("cannot create temp file for %1: %2").arg(name, errnoText());
            return false;
        }
    }
    if (fd < 0) {
        *error = QStringLiteral("cannot allocate a unique temp file name for %1").arg(name);
        return false;
    }

    auto abandon = [&](const QString &message) {
        *error = message;
        ::close(fd);
        ::unlinkat(dirFd, tmpName.toLocal8Bit().constData(), 0);
        return false;
    };

    qint64 written = 0;
    while (written < content.size()) {
        const ssize_t n = ::write(fd, content.constData() + written, static_cast<size_t>(content.size() - written));
        if (n <= 0) {
            return abandon(QStringLiteral("write to temp file for %1 failed: %2").arg(name, errnoText()));
        }
        written += n;
    }
    if (::fchown(fd, 0, 0) != 0 || ::fchmod(fd, modeFor(kind)) != 0) {
        return abandon(QStringLiteral("cannot secure temp file for %1: %2").arg(name, errnoText()));
    }
    if (::fsync(fd) != 0) {
        return abandon(QStringLiteral("cannot fsync temp file for %1: %2").arg(name, errnoText()));
    }
    ::close(fd);

    const QByteArray tmpRaw = tmpName.toLocal8Bit();
    const QByteArray nameRaw = name.toLocal8Bit();
    if (::renameat(dirFd, tmpRaw.constData(), dirFd, nameRaw.constData()) != 0) {
        *error = QStringLiteral("cannot install %1: %2").arg(name, errnoText());
        ::unlinkat(dirFd, tmpRaw.constData(), 0);
        return false;
    }
    if (::fsync(dirFd) != 0) {
        *error = QStringLiteral("cannot fsync directory after installing %1: %2").arg(name, errnoText());
        return false;
    }
    return true;
}

bool durableUnlink(int dirFd, const QString &name, bool allowMissing, QString *error)
{
    if (!isSingleComponent(name)) {
        *error = QStringLiteral("invalid file component: %1").arg(name);
        return false;
    }
    const QByteArray raw = name.toLocal8Bit();
    if (::unlinkat(dirFd, raw.constData(), 0) != 0) {
        if (errno == ENOENT && allowMissing) {
            return true;
        }
        *error = QStringLiteral("cannot remove %1: %2").arg(name, errnoText());
        return false;
    }
    if (::fsync(dirFd) != 0) {
        *error = QStringLiteral("cannot fsync directory after removing %1: %2").arg(name, errnoText());
        return false;
    }
    return true;
}

bool durableRemoveTree(int parentFd, const QString &name, QString *error)
{
    if (!isSingleComponent(name)) {
        *error = QStringLiteral("invalid directory component: %1").arg(name);
        return false;
    }
    QString openError;
    const int dirFd = openVerifiedDir(parentFd, name, &openError);
    if (dirFd < 0) {
        if (openError.isEmpty()) {
            return true; // already absent -- idempotent
        }
        *error = openError;
        return false;
    }

    const int dupFd = ::dup(dirFd);
    if (dupFd < 0) {
        *error = QStringLiteral("cannot duplicate descriptor for %1: %2").arg(name, errnoText());
        ::close(dirFd);
        return false;
    }
    DIR *dir = ::fdopendir(dupFd);
    if (!dir) {
        *error = QStringLiteral("cannot list %1: %2").arg(name, errnoText());
        ::close(dupFd);
        ::close(dirFd);
        return false;
    }
    errno = 0;
    while (struct dirent *entry = ::readdir(dir)) {
        const QByteArray entryName(entry->d_name);
        if (entryName == "." || entryName == "..") {
            errno = 0;
            continue;
        }
        struct stat entrySt {};
        if (::fstatat(dirFd, entry->d_name, &entrySt, AT_SYMLINK_NOFOLLOW) != 0) {
            ::closedir(dir);
            ::close(dirFd);
            *error = QStringLiteral("cannot inspect %1/%2: %3")
                         .arg(name, QString::fromLocal8Bit(entryName), errnoText());
            return false;
        }
        if (S_ISDIR(entrySt.st_mode)) {
            ::closedir(dir);
            ::close(dirFd);
            *error = QStringLiteral("refusing to recurse into unexpected subdirectory %1/%2").arg(name, QString::fromLocal8Bit(entryName));
            return false;
        }
        // Every tree this primitive clears contains only nasmount's private
        // root-owned 0600 artifacts (credentials, runtime IDs, and the root
        // lock). Refuse a symlink, device, foreign owner, or drifted mode
        // instead of turning a cleanup operation into an unreviewed deletion
        // primitive.
        if (!S_ISREG(entrySt.st_mode) || entrySt.st_uid != 0 || (entrySt.st_mode & 07777) != 0600) {
            ::closedir(dir);
            ::close(dirFd);
            *error = QStringLiteral("refusing unsafe entry %1/%2").arg(name, QString::fromLocal8Bit(entryName));
            return false;
        }
        if (::unlinkat(dirFd, entry->d_name, 0) != 0) {
            ::closedir(dir);
            ::close(dirFd);
            *error = QStringLiteral("cannot remove %1/%2: %3")
                         .arg(name, QString::fromLocal8Bit(entryName), errnoText());
            return false;
        }
        errno = 0;
    }
    const int savedErrno = errno;
    ::closedir(dir); // also closes dupFd
    if (savedErrno != 0) {
        ::close(dirFd);
        *error = QStringLiteral("cannot enumerate %1: %2").arg(name, QString::fromLocal8Bit(strerror(savedErrno)));
        return false;
    }
    if (::fsync(dirFd) != 0) {
        *error = QStringLiteral("cannot fsync %1 after clearing it: %2").arg(name, errnoText());
        ::close(dirFd);
        return false;
    }
    ::close(dirFd);

    const QByteArray raw = name.toLocal8Bit();
    if (::unlinkat(parentFd, raw.constData(), AT_REMOVEDIR) != 0) {
        *error = QStringLiteral("cannot remove directory %1: %2").arg(name, errnoText());
        return false;
    }
    if (::fsync(parentFd) != 0) {
        *error = QStringLiteral("cannot fsync parent directory after removing %1: %2").arg(name, errnoText());
        return false;
    }
    return true;
}

} // namespace Root::DurableFs
