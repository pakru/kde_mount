/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "credentialstore.h"
#include "durablefs.h"
#include "unitspec.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

namespace Root::CredentialStore
{

namespace
{

/** Opens (creating as needed) the root-owned credential directory,
 *  /etc/nasmount. A share's credential is persistent; there is no second
 *  location. */
int openCredentialDir(QString *error)
{
    const int rootFd = DurableFs::openSystemRoot(QStringLiteral("/etc"), error);
    if (rootFd < 0) {
        return -1;
    }
    const int dirFd =
        DurableFs::createAndVerifyDir(rootFd, QStringLiteral("nasmount"), DurableFs::ArtifactKind::Directory, error);
    ::close(rootFd);
    return dirFd;
}

QString fileNameFor(const QString &id)
{
    return QStringLiteral("%1.cred").arg(id);
}

bool validateFields(const QString &username, const QString &domain, const QString &password, QString *error)
{
    if (UnitSpec::hasControlChars(username) || UnitSpec::hasControlChars(domain)
        || UnitSpec::hasControlChars(password)) {
        *error = QStringLiteral("credential fields contain control characters");
        return false;
    }
    if (username.toUtf8().size() > UnitSpec::MaxCredentialFieldBytes
        || domain.toUtf8().size() > UnitSpec::MaxCredentialFieldBytes
        || password.toUtf8().size() > UnitSpec::MaxCredentialFieldBytes) {
        *error = QStringLiteral("a credential field exceeds %1 bytes").arg(UnitSpec::MaxCredentialFieldBytes);
        return false;
    }
    // write() is only ever called for an authenticated share (guest skips
    // it entirely and calls assertAbsent() instead) -- an empty password
    // here is never "no password", it is a caller that failed to resolve
    // one. Reject it rather than durably writing a credentials file with no
    // password= line at all, which mount.cifs(8) would then try against
    // the real server.
    if (!username.isEmpty() && password.isEmpty()) {
        *error = QStringLiteral("a password is required when a username is given");
        return false;
    }
    return true;
}

} // namespace

bool write(const QString &id, const QString &username, const QString &domain,
          const QString &password, QString *error)
{
    if (!UnitValue::isValidShareId(id)) {
        *error = QStringLiteral("invalid share id");
        return false;
    }
    if (!validateFields(username, domain, password, error)) {
        return false;
    }

    QString content = QStringLiteral("username=%1\n").arg(username);
    if (!password.isEmpty()) {
        content += QStringLiteral("password=%1\n").arg(password);
    }
    if (!domain.isEmpty()) {
        content += QStringLiteral("domain=%1\n").arg(domain);
    }
    const QByteArray bytes = content.toUtf8();
    if (bytes.size() > UnitSpec::MaxCredentialFileBytes) {
        *error = QStringLiteral("credential file exceeds %1 bytes").arg(UnitSpec::MaxCredentialFileBytes);
        return false;
    }

    const int dirFd = openCredentialDir(error);
    if (dirFd < 0) {
        return false;
    }
    const bool ok = DurableFs::durableReplace(dirFd, fileNameFor(id), bytes, DurableFs::ArtifactKind::SensitiveFile,
                                              error);
    ::close(dirFd);
    return ok;
}

bool remove(const QString &id, bool allowMissing, QString *error)
{
    if (!UnitValue::isValidShareId(id)) {
        *error = QStringLiteral("invalid share id");
        return false;
    }
    const int dirFd = openCredentialDir(error);
    if (dirFd < 0) {
        return false;
    }
    const bool ok = DurableFs::durableUnlink(dirFd, fileNameFor(id), allowMissing, error);
    ::close(dirFd);
    return ok;
}

bool healthy(const QString &id, QString *error)
{
    if (!UnitValue::isValidShareId(id)) {
        *error = QStringLiteral("invalid share id");
        return false;
    }
    const int dirFd = openCredentialDir(error);
    if (dirFd < 0) {
        return false;
    }
    const QByteArray raw = fileNameFor(id).toLocal8Bit();
    const int fd = ::openat(dirFd, raw.constData(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    ::close(dirFd);
    if (fd < 0) {
        *error = (errno == ENOENT) ? QStringLiteral("credential file is missing")
                                   : QStringLiteral("cannot open credential file: %1")
                                         .arg(QString::fromLocal8Bit(strerror(errno)));
        return false;
    }
    QString verifyError;
    const bool ok = DurableFs::verifyDescriptor(fd, DurableFs::FileKind::Regular, 0,
                                                DurableFs::modeFor(DurableFs::ArtifactKind::SensitiveFile),
                                                &verifyError);
    ::close(fd);
    if (!ok) {
        *error = verifyError;
    }
    return ok;
}

bool assertAbsent(const QString &id, QString *error)
{
    if (!UnitValue::isValidShareId(id)) {
        *error = QStringLiteral("invalid share id");
        return false;
    }
    const int dirFd = openCredentialDir(error);
    if (dirFd < 0) {
        return false;
    }
    const QByteArray raw = fileNameFor(id).toLocal8Bit();
    const int fd = ::openat(dirFd, raw.constData(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    ::close(dirFd);
    if (fd < 0) {
        return errno == ENOENT;
    }
    ::close(fd);
    return false; // exists -- guest must never have a credential artifact
}

} // namespace Root::CredentialStore
