/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "systemdops.h"

#include <QProcess>

namespace Root::SystemdOps
{

namespace
{
constexpr int TimeoutMs = 60000;
constexpr qsizetype MaxOutputBytes = 65536;

CommandRunner &activeRunner()
{
    static CommandRunner runner;
    return runner;
}
} // namespace

int runCommand(const QString &program, const QStringList &args, QString *output)
{
    QProcess proc;
    proc.start(program, args);
    if (!proc.waitForFinished(TimeoutMs)) {
        proc.kill();
        proc.waitForFinished(5000);
        *output = QStringLiteral("%1 timed out").arg(program);
        return -1;
    }
    QByteArray combined = proc.readAllStandardError();
    combined += proc.readAllStandardOutput();
    if (combined.size() > MaxOutputBytes) {
        combined = combined.left(static_cast<int>(MaxOutputBytes));
        combined += "... (truncated)";
    }
    *output = QString::fromLocal8Bit(combined).trimmed();
    return proc.exitCode();
}

void setCommandRunner(CommandRunner runner)
{
    activeRunner() = std::move(runner);
}

namespace
{
int dispatch(const QString &program, const QStringList &args, QString *output)
{
    const CommandRunner &runner = activeRunner();
    return runner ? runner(program, args, output) : runCommand(program, args, output);
}
} // namespace

bool daemonReload(QString *error)
{
    QString out;
    if (dispatch(QStringLiteral("systemctl"), {QStringLiteral("daemon-reload")}, &out) != 0) {
        *error = QStringLiteral("daemon-reload failed: %1").arg(out);
        return false;
    }
    return true;
}

bool start(const QString &unit, QString *error)
{
    QString out;
    if (dispatch(QStringLiteral("systemctl"), {QStringLiteral("start"), unit}, &out) != 0) {
        *error = QStringLiteral("failed to start %1: %2").arg(unit, out);
        return false;
    }
    return true;
}

bool stop(const QString &unit, QString *error)
{
    QString out;
    if (dispatch(QStringLiteral("systemctl"), {QStringLiteral("stop"), unit}, &out) != 0) {
        *error = QStringLiteral("failed to stop %1: %2").arg(unit, out);
        return false;
    }
    return true;
}

bool showProperty(const QString &unit, const QString &property, QString *value, QString *error)
{
    QString out;
    const int rc = dispatch(QStringLiteral("systemctl"),
                            {QStringLiteral("show"), unit, QStringLiteral("--property=%1").arg(property),
                             QStringLiteral("--value")},
                            &out);
    if (rc != 0) {
        *error = QStringLiteral("systemctl show %1 --property=%2 failed: %3").arg(unit, property, out);
        return false;
    }
    *value = out;
    return true;
}

void stopQuiet(const QStringList &units)
{
    if (units.isEmpty()) {
        return;
    }
    QString out;
    QStringList args{QStringLiteral("stop")};
    args += units;
    dispatch(QStringLiteral("systemctl"), args, &out);
}

} // namespace Root::SystemdOps
