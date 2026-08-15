/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include <QString>
#include <QStringList>

namespace Session
{

/** Validates CMake's user-writable install manifest against the finite set
 * of paths this project installs. Returns the exact paths to remove. */
bool validateInstallManifest(const QString &manifestPath, QStringList *targets, QString *error);

} // namespace Session
