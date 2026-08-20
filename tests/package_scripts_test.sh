#!/bin/bash
# Rootless static/functional checks for native package shell entry points.
# SPDX-License-Identifier: GPL-3.0-or-later

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
uninstaller="$repo_root/packaging/nasmount-uninstall.sh"

bash -n "$uninstaller"
# Sourcing exposes only pure command selection; main is guarded and therefore
# cannot request authorization during CTest.
source "$uninstaller"

[ "$(package_manager_binary deb)" = /usr/bin/apt-get ]
[ "$(package_manager_binary rpm)" = /usr/bin/dnf ]
mapfile -t deb_remove < <(package_manager_remove_arguments deb)
mapfile -t rpm_remove < <(package_manager_remove_arguments rpm)
[ "${deb_remove[*]}" = "remove nasmount" ]
[ "${rpm_remove[*]}" = "remove --no-autoremove nasmount" ]
if package_manager_binary source >/dev/null 2>&1; then
    echo "ERROR: source installs must not select a package manager" >&2
    exit 1
fi
if package_manager_remove_arguments source >/dev/null 2>&1; then
    echo "ERROR: source installs must not select native removal arguments" >&2
    exit 1
fi

grep -Fq '"$NASMOUNT_CLEANUP" --manifest "$NASMOUNT_MANIFEST"' "$uninstaller"
cleanup_line=$(grep -nF '"$NASMOUNT_CLEANUP" --manifest "$NASMOUNT_MANIFEST"' "$uninstaller" | cut -d: -f1)
remove_line=$(grep -nF 'remove_native_package "$family"' "$uninstaller" | tail -1 | cut -d: -f1)
[ "$cleanup_line" -lt "$remove_line" ] || {
    echo "ERROR: native package removal appears before authenticated cleanup" >&2
    exit 1
}

echo "Native package shell checks passed."
