/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "kcmnasmount.h"

#include <KPluginFactory>

K_PLUGIN_CLASS_WITH_JSON(KcmNasmount, "kcm_nasmount.json")

KcmNasmount::KcmNasmount(QObject *parent, const KPluginMetaData &metaData)
    : KQuickConfigModule(parent, metaData)
    , m_model(new Session::MountModel(this))
    , m_actions(new Session::MountActions(this))
{
    setButtons(KAbstractConfigModule::NoAdditionalButton);
    // Every action already took effect by the time it reports finished(); the
    // only thing left to do is re-read what actually happened.
    connect(m_actions, &Session::MountActions::finished, this,
           [this](const QString &, const QString &, bool, const QString &) { m_model->refresh(); });
}

KcmNasmount::~KcmNasmount() = default;

Session::MountModel *KcmNasmount::shareModel() const
{
    return m_model;
}

Session::MountActions *KcmNasmount::actions() const
{
    return m_actions;
}

#include "kcmnasmount.moc"
