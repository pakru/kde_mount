/*
 * runtimefiles — root-owned runtime records below /run/nasmount: the
 * automount instance id recorded at arm time (plan §2.1.4, §2.3.5-6).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Writes only; reading the recorded automount id back is unprivileged (any
 * process may read a file under /run/nasmount/automount-ids to compute
 * Verify::ActivationTrust) and stays in nasmount-core as
 * Verify::readRecordedAutomountId(). Everything here is root-only: only the
 * privileged helper (and the boot coordinator) ever creates, moves, or
 * removes one of these records.
 */

#pragma once

#include <QString>

#include <cstdint>

namespace Root::RuntimeFiles
{

/**
 * Durably records `id` as the trusted automount instance for `unitName`
 * (design §6.4's "where identity still applies"). Overwrites any previous
 * record for the same unit.
 */
bool writeAutomountId(const QString &unitName, uint64_t id, QString *error);

/** Durably removes a recorded automount id. Missing records are accepted so
 *  recovery can repeat the operation after a crash. */
bool removeAutomountId(const QString &unitName, QString *error);

} // namespace Root::RuntimeFiles
