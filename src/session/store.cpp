/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "store.h"

#include <KConfigGroup>
#include <KSharedConfig>
#include <KWallet>

#include <QFile>
#include <QStandardPaths>

using KWallet::Wallet;

namespace
{

const QLatin1String ConfigName("nasmountrc");
const QLatin1String SharesGroup("Shares");
const QLatin1String WalletFolder("nasmount");

Wallet *openWallet()
{
    // Synchronous open: this runs off the GUI thread (Session::MountActions
    // runs it via QtConcurrent), and there is nothing useful to do until the
    // wallet is available either way.
    return Wallet::openWallet(Wallet::NetworkWallet(), 0, Wallet::Synchronous);
}

/** Reads one share group's fields, whatever is present. Does not decide
 *  corrupt/exists -- the caller does, since "exists" also depends on whether
 *  the group is present at all. */
Store::Share readShareFields(const KConfigGroup &g, const QString &id)
{
    Store::Share s;
    s.id = id;
    s.mountPoint = g.readEntry("MountPoint", QString());
    s.unc = g.readEntry("Unc", QString());
    s.username = g.readEntry("Username", QString());
    s.domain = g.readEntry("Domain", QString());
    s.reconnect = g.readEntry("Reconnect", false);
    s.mode = (g.readEntry("Mode", QStringLiteral("session")) == QStringLiteral("system"))
        ? UnitValue::CredentialMode::System
        : UnitValue::CredentialMode::Session;
    s.rememberPassword = g.readEntry("RememberPassword", false);
    return s;
}

void writeShareFields(KConfigGroup &g, const Store::Share &share)
{
    g.writeEntry("MountPoint", share.mountPoint);
    g.writeEntry("Unc", share.unc);
    g.writeEntry("Username", share.username);
    g.writeEntry("Domain", share.domain);
    g.writeEntry("Reconnect", share.reconnect);
    g.writeEntry("Mode", share.mode == UnitValue::CredentialMode::System ? QStringLiteral("system")
                                                                         : QStringLiteral("session"));
    g.writeEntry("RememberPassword", share.rememberPassword);
}

Store::Snapshot readSnapshot(const KConfigGroup &root, const QString &id)
{
    Store::Snapshot snap;
    if (!root.hasGroup(id)) {
        return snap;
    }
    const KConfigGroup g = root.group(id);
    snap.share = readShareFields(g, id);
    snap.generation = static_cast<quint64>(g.readEntry("Generation", 0LL));
    snap.exists = true;
    snap.corrupt = snap.share.unc.isEmpty() || snap.share.mountPoint.isEmpty();
    return snap;
}

} // namespace

namespace Store
{

QList<Snapshot> shareSnapshots()
{
    QList<Snapshot> result;
    auto config = KSharedConfig::openConfig(ConfigName);
    config->reparseConfiguration();
    const KConfigGroup root = config->group(SharesGroup);
    const QStringList groups = root.groupList();
    result.reserve(groups.size());
    for (const QString &id : groups) {
        result.append(readSnapshot(root, id));
    }
    return result;
}

QList<Share> shares()
{
    QList<Share> result;
    for (const Snapshot &snap : shareSnapshots()) {
        if (!snap.corrupt) {
            result.append(snap.share);
        }
    }
    return result;
}

Snapshot snapshotById(const QString &id)
{
    auto config = KSharedConfig::openConfig(ConfigName);
    config->reparseConfiguration();
    const KConfigGroup root = config->group(SharesGroup);
    return readSnapshot(root, id);
}

CommitResult commitShare(const Share &share, quint64 expectedGeneration, QString *error)
{
    Q_ASSERT(!share.id.isEmpty());
    auto config = KSharedConfig::openConfig(ConfigName);
    // Re-read immediately before the compare, not the caller's possibly-stale
    // in-memory snapshot: another cooperating process may have committed
    // since this caller's own read (plan §1.6.3-4). This check is meaningful
    // because the caller is expected to hold Session::UserLock across both;
    // it catches a lock-discipline bug, it is not a substitute for the lock.
    config->reparseConfiguration();
    const KConfigGroup root = config->group(SharesGroup);
    const Snapshot current = readSnapshot(root, share.id);
    if (current.exists != (expectedGeneration != 0) || (current.exists && current.generation != expectedGeneration)) {
        *error = QStringLiteral("share record changed since it was read; reload and retry");
        return CommitResult::GenerationConflict;
    }

    KConfigGroup mutableRoot = config->group(SharesGroup);
    KConfigGroup g = mutableRoot.group(share.id);
    writeShareFields(g, share);
    g.writeEntry("Generation", static_cast<qlonglong>(expectedGeneration + 1));
    if (!config->sync()) {
        *error = QStringLiteral("could not write configuration to disk");
        return CommitResult::SyncFailed;
    }
    return CommitResult::Ok;
}

bool removeShare(const QString &id)
{
    // Wallet first, config group second (plan §13.3): a crash between the
    // two then leaves, at worst, a visible orphaned config record with no
    // password -- never an unindexed wallet secret nothing points at.
    if (!removePassword(id)) {
        return false;
    }
    auto config = KSharedConfig::openConfig(ConfigName);
    config->reparseConfiguration();
    KConfigGroup root = config->group(SharesGroup);
    if (root.hasGroup(id)) {
        root.deleteGroup(id);
        if (!config->sync()) {
            return false;
        }
    }
    return true;
}

bool walletIsOpen()
{
    return Wallet::isOpen(Wallet::NetworkWallet());
}

bool readPassword(const QString &id, QString *password, bool onlyIfUnlocked)
{
    if (id.isEmpty()) {
        return false;
    }
    // openWallet() pops an unlock dialog if the wallet is locked. The
    // supervisor's unattended arming at sign-in must not do that; it checks
    // onlyIfUnlocked and skips rather than blocking on a prompt (plan §7.1).
    if (onlyIfUnlocked && !walletIsOpen()) {
        return false;
    }
    std::unique_ptr<Wallet> wallet(openWallet());
    if (!wallet || !wallet->isOpen()) {
        return false;
    }
    if (!wallet->hasFolder(WalletFolder) || !wallet->setFolder(WalletFolder)) {
        return false;
    }
    return wallet->readPassword(id, *password) == 0;
}

bool writePassword(const QString &id, const QString &password)
{
    if (id.isEmpty()) {
        return false;
    }
    std::unique_ptr<Wallet> wallet(openWallet());
    if (!wallet || !wallet->isOpen()) {
        return false;
    }
    if (!wallet->hasFolder(WalletFolder) && !wallet->createFolder(WalletFolder)) {
        return false;
    }
    if (!wallet->setFolder(WalletFolder)) {
        return false;
    }
    return wallet->writePassword(id, password) == 0;
}

bool removePassword(const QString &id)
{
    if (id.isEmpty()) {
        return true;
    }
    std::unique_ptr<Wallet> wallet(openWallet());
    if (!wallet || !wallet->isOpen()) {
        // The caller cannot prove whether this id has an entry while the
        // wallet is unavailable. Keep the config record as the index to any
        // possible secret and let the user retry after unlocking the wallet.
        return false;
    }
    if (!wallet->hasFolder(WalletFolder)) {
        return true;
    }
    if (!wallet->setFolder(WalletFolder)) {
        return false;
    }
    return wallet->removeEntry(id) == 0;
}

bool purgeApplicationData(QString *error)
{
    std::unique_ptr<Wallet> wallet(openWallet());
    if (!wallet || !wallet->isOpen()) {
        *error = QStringLiteral("KWallet is unavailable or was not unlocked");
        return false;
    }
    if (wallet->hasFolder(WalletFolder) && !wallet->removeFolder(WalletFolder)) {
        *error = QStringLiteral("could not remove the nasmount KWallet folder");
        return false;
    }

    const QString configDir = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation);
    if (configDir.isEmpty()) {
        *error = QStringLiteral("the user configuration directory is unavailable");
        return false;
    }
    const QString configPath = configDir + QStringLiteral("/nasmountrc");
    if (QFile::exists(configPath) && !QFile::remove(configPath)) {
        *error = QStringLiteral("could not remove %1").arg(configPath);
        return false;
    }
    return true;
}

} // namespace Store
