/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QString>
#include <QStringList>

namespace Session
{

/** Validates a CMake or package-owned cleanup manifest against the finite,
 * complete set of paths this project installs. Returns the exact paths. */
bool validateInstallManifest(const QString &manifestPath, QStringList *targets, QString *error);

} // namespace Session
