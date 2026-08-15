/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "mountdialog.h"
#include "mountactions.h"
#include "store.h"
#include "unitspec.h"
#include "unitvalue.h"
#include "verify.h"

#include <KGuiItem>
#include <KLocalizedString>
#include <KMessageBox>
#include <KStandardGuiItem>

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QUrl>
#include <QVBoxLayout>

namespace
{

/**
 * Suggests ~/<ShareName> for the share being mounted.
 *
 * The share's own name is kept as-is rather than upper-cased or filed under a
 * fixed parent folder: those are personal conventions, and the name the server
 * already uses is the least surprising default.
 */
QString suggestMountpoint(const QString &unc)
{
    QString leaf = unc;
    while (leaf.endsWith(QLatin1Char('/'))) {
        leaf.chop(1);
    }
    leaf = leaf.section(QLatin1Char('/'), -1);

    leaf.replace(QLatin1Char('/'), QLatin1Char('_'));
    leaf = leaf.trimmed();
    if (leaf.isEmpty() || leaf == QStringLiteral(".") || leaf == QStringLiteral("..")) {
        leaf = QStringLiteral("Share");
    }
    return QDir::homePath() + QLatin1Char('/') + leaf;
}

/** Coarse state text for a saved share, without needing the full KCM model. */
QString describeState(const QString &mountPoint)
{
    UnitValue::UnitPaths paths;
    QString err;
    if (!UnitValue::unitPathsFor(mountPoint, &paths, &err)) {
        return QStringLiteral("unknown");
    }
    const auto def = Verify::inspectDefinition(paths, ::getuid(), mountPoint);
    if (def.state != Verify::Definition::Pair) {
        return QStringLiteral("needs attention");
    }
    const Verify::RuntimeSnapshot snap = Verify::inspectRuntime(paths.unitName, mountPoint, def.what);
    if (snap.mount == Verify::MountState::Present && snap.verification == Verify::VerificationState::Match) {
        return QStringLiteral("mounted");
    }
    if (snap.mount == Verify::MountState::Indeterminate) {
        return QStringLiteral("needs attention");
    }
    if (snap.automount == Verify::AutomountState::Active) {
        return (snap.activationTrust == Verify::ActivationTrust::Trusted)
            ? QStringLiteral("armed — mounts on first access")
            : QStringLiteral("needs attention");
    }
    return QStringLiteral("defined, not armed");
}

} // namespace

bool MountDialog::parseSmbUrl(const QString &raw, QString *unc, QString *user, QString *error)
{
    // Dolphin substitutes %u with the URL as displayed, which for a share like
    // "Media Library" contains literal spaces. QUrl::StrictMode rejects those
    // outright ("character ' ' not permitted"), so it cannot be used alone.
    //
    // Parse strictly when the URL is already well-formed, and fall back to
    // tolerant parsing — which percent-encodes the offending characters rather
    // than reinterpreting the URL's structure — otherwise. Safety does not rest
    // on the parsing mode: the component checks below reject anything unexpected,
    // and the helper re-validates everything regardless.
    QUrl url(raw, QUrl::StrictMode);
    if (!url.isValid()) {
        // One caveat before falling back: a single malformed %-escape puts Qt
        // into repair mode for *every* percent sign in the URL, so
        // ".../Media%20Library/100%" would yield a literal "Media%20Library"
        // rather than "Media Library" — a different share name on the same
        // server. A correctly displayed literal percent is already "%25", so
        // refuse rather than guess.
        static const QRegularExpression badEscape(
            QStringLiteral("%(?![0-9A-Fa-f]{2})"));
        if (badEscape.match(raw).hasMatch()) {
            *error = i18n("This URL contains a malformed %1 escape: %2",
                          QStringLiteral("%"), raw);
            return false;
        }
        url = QUrl(raw, QUrl::TolerantMode);
    }
    if (!url.isValid()) {
        *error = i18n("Not a valid URL: %1 (%2)", raw, url.errorString());
        return false;
    }
    if (url.scheme() != QStringLiteral("smb")) {
        *error = i18n("Not an smb:// URL: %1", raw);
        return false;
    }
    // password().isEmpty() misses "smb://user:@host/share", where the component
    // is present but empty; check the raw separator instead.
    const bool hasPasswordComponent =
        url.userInfo(QUrl::FullyEncoded).contains(QLatin1Char(':'));
    if (url.hasQuery() || url.hasFragment() || hasPasswordComponent || url.port() != -1) {
        *error = i18n("This URL has parts that are not supported here "
                      "(port, password, query or fragment): %1", raw);
        return false;
    }
    const QString host = url.host();
    if (host.isEmpty()) {
        *error = i18n("No host in URL: %1", raw);
        return false;
    }
    if (host.contains(QLatin1Char(':'))) {
        *error = i18n("IPv6 hosts are not supported yet: %1", host);
        return false;
    }

    QString path = url.path(QUrl::FullyDecoded);
    while (path.startsWith(QLatin1Char('/'))) {
        path.remove(0, 1);
    }
    while (path.endsWith(QLatin1Char('/'))) {
        path.chop(1);
    }
    if (path.isEmpty()) {
        *error = i18n("This URL points at a server, not a share — open a share first.");
        return false;
    }

    const QString decodedUser = url.userName(QUrl::FullyDecoded);
    if (UnitSpec::hasControlChars(host) || UnitSpec::hasControlChars(path)
        || UnitSpec::hasControlChars(decodedUser)) {
        *error = i18n("The URL contains control characters.");
        return false;
    }

    *unc = QStringLiteral("//%1/%2").arg(host, path);
    *user = decodedUser;
    return true;
}

MountDialog::MountDialog(const QString &unc, const QString &suggestedUser, QWidget *parent)
    : QDialog(parent)
    , m_unc(unc)
    , m_actions(new Session::MountActions(this))
{
    connect(m_actions, &Session::MountActions::finished, this, &MountDialog::onActionFinished);

    setWindowTitle(i18n("Mount as Network Drive"));
    setMinimumWidth(560);

    auto *layout = new QVBoxLayout(this);

    auto *header = new QLabel(QStringLiteral("<b>%1</b>").arg(unc.toHtmlEscaped()), this);
    header->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(header);

    // Prefer whatever was saved for this share last time.
    Store::Share saved;
    bool haveSaved = false;
    for (const Store::Share &s : Store::shares()) {
        if (s.unc == unc) {
            saved = s;
            haveSaved = true;
            break;
        }
    }
    if (haveSaved) {
        m_existingId = saved.id;
        m_stateText = describeState(saved.mountPoint);
    }

    if (haveSaved) {
        auto *note = new QLabel(
            i18n("This share is already saved, at <b>%1</b> (%2).", saved.mountPoint.toHtmlEscaped(), m_stateText),
            this);
        note->setWordWrap(true);
        layout->addWidget(note);
    }

    // --- location ---------------------------------------------------------
    auto *locBox = new QGroupBox(i18n("Location"), this);
    auto *locForm = new QFormLayout(locBox);
    const QString initialPath = haveSaved ? saved.mountPoint : suggestMountpoint(unc);
    m_path = new QLineEdit(initialPath, locBox);
    m_path->setEnabled(!haveSaved);
    auto *browseButton = new QPushButton(i18n("Browse…"), locBox);
    browseButton->setEnabled(!haveSaved);
    connect(browseButton, &QPushButton::clicked, this, &MountDialog::browse);
    auto *pathRow = new QHBoxLayout;
    pathRow->addWidget(m_path);
    pathRow->addWidget(browseButton);
    locForm->addRow(i18n("Mount point:"), pathRow);
    layout->addWidget(locBox);

    // --- credentials ------------------------------------------------------
    auto *credBox = new QGroupBox(i18n("Credentials"), this);
    auto *credForm = new QFormLayout(credBox);
    m_user = new QLineEdit(haveSaved ? saved.username : suggestedUser, credBox);
    m_user->setPlaceholderText(i18n("leave empty for guest access"));
    m_user->setEnabled(!haveSaved);
    m_password = new QLineEdit(credBox);
    m_password->setEchoMode(QLineEdit::Password);
    m_password->setEnabled(!haveSaved);
    m_domain = new QLineEdit(haveSaved ? saved.domain : QString(), credBox);
    m_domain->setPlaceholderText(i18n("optional"));
    m_domain->setEnabled(!haveSaved);
    credForm->addRow(i18n("Username:"), m_user);
    credForm->addRow(i18n("Password:"), m_password);
    credForm->addRow(i18n("Domain:"), m_domain);

    m_remember = new QCheckBox(i18n("Remember password in KWallet"), credBox);
    m_remember->setChecked(true);
    m_remember->setEnabled(!haveSaved);
    credForm->addRow(m_remember);
    layout->addWidget(credBox);

    // --- behaviour --------------------------------------------------------
    auto *behaveBox = new QGroupBox(i18n("Behaviour"), this);
    auto *behaveForm = new QFormLayout(behaveBox);
    m_reconnect = new QCheckBox(i18n("Arm automatically at sign-in"), behaveBox);
    m_reconnect->setChecked(haveSaved ? saved.reconnect : true);
    m_reconnect->setEnabled(!haveSaved);
    behaveForm->addRow(m_reconnect);

    m_note = new QLabel(
        haveSaved ? i18n("<i>To change settings for a saved share, use System Settings → Network Mounts. "
                        "Unmount below removes it entirely.</i>")
                 : i18n("<i>Mounted on demand, on first access, with an idle unmount. The password is kept in "
                        "KWallet — never in a file on disk — and nothing is armed before you sign in.</i>"),
        behaveBox);
    m_note->setWordWrap(true);
    behaveForm->addRow(m_note);
    layout->addWidget(behaveBox);

    // --- buttons ----------------------------------------------------------
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
    if (!haveSaved) {
        m_mountButton = buttons->addButton(i18n("Mount"), QDialogButtonBox::AcceptRole);
        connect(m_mountButton, &QPushButton::clicked, this, &MountDialog::doMount);
    } else {
        m_unmountButton = buttons->addButton(i18n("Unmount"), QDialogButtonBox::DestructiveRole);
        connect(m_unmountButton, &QPushButton::clicked, this, &MountDialog::doUnmount);
    }
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}

void MountDialog::browse()
{
    const QString chosen =
        QFileDialog::getExistingDirectory(this, i18n("Choose mount point"), m_path->text());
    if (!chosen.isEmpty()) {
        m_path->setText(chosen);
    }
}

void MountDialog::doMount()
{
    const QString path = m_path->text().trimmed();
    if (!path.startsWith(QLatin1Char('/'))) {
        KMessageBox::error(this, i18n("The mount point must be an absolute path."));
        return;
    }
    if (m_mountButton) {
        m_mountButton->setEnabled(false);
    }
    m_actions->addShare(m_unc, path, m_user->text().trimmed(), m_domain->text().trimmed(), m_password->text(),
                        m_remember->isChecked(), m_reconnect->isChecked());
}

void MountDialog::doUnmount()
{
    const auto confirm = KMessageBox::questionTwoActions(
        this, i18n("Remove this saved share?\n\nIf it is currently mounted, it will be unmounted first. The "
                  "mount point directory itself is left in place."),
        i18n("Remove"), KGuiItem(i18n("Remove")), KStandardGuiItem::cancel());
    if (confirm != KMessageBox::PrimaryAction) {
        return;
    }
    if (m_unmountButton) {
        m_unmountButton->setEnabled(false);
    }
    m_actions->deleteShare(m_existingId);
}

void MountDialog::onActionFinished(const QString &id, const QString &kind, bool success, const QString &message)
{
    Q_UNUSED(id);
    if (!success) {
        if (m_mountButton) {
            m_mountButton->setEnabled(true);
        }
        if (m_unmountButton) {
            m_unmountButton->setEnabled(true);
        }
        KMessageBox::error(this, message);
        return;
    }
    KMessageBox::information(this, message,
                             kind == QStringLiteral("add") ? i18n("Added") : i18n("Removed"));
    accept();
}
