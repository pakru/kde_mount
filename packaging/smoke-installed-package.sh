#!/bin/bash
# Inspect one package after native installation in a disposable target root.
# SPDX-License-Identifier: GPL-3.0-or-later

set -euo pipefail

family=${1:-}
expected_version=${2:-}
[[ "$family" =~ ^(deb|rpm)$ ]] && [[ "$expected_version" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || {
    echo "Usage: $0 deb|rpm MAJOR.MINOR.PATCH" >&2
    exit 2
}

case "$family" in
    deb) mapfile -t owned_paths < <(dpkg-query -L nasmount) ;;
    rpm) mapfile -t owned_paths < <(rpm -ql nasmount) ;;
esac

elf_count=0
for path in "${owned_paths[@]}"; do
    [ -f "$path" ] || continue
    [ "$(stat -c '%u:%g' -- "$path")" = 0:0 ] || {
        echo "ERROR: package file is not root-owned: $path" >&2
        exit 1
    }
    if file -b -- "$path" | grep -q '^ELF '; then
        elf_count=$((elf_count + 1))
        if ldd "$path" | grep -q 'not found'; then
            echo "ERROR: unresolved library in $path" >&2
            exit 1
        fi
    fi
done
[ "$elf_count" -gt 0 ]

[ "$(tr -d '[:space:]' < /usr/share/nasmount/package-family)" = "$family" ]
[ -f /usr/lib/systemd/system/nasmount-boot.service ]
/usr/bin/nasmount-cleanup \
    --manifest /usr/share/nasmount/cleanup-manifest.txt --validate-only
if [ "$family" = rpm ]; then
    [ -f /usr/lib/systemd/system-preset/90-nasmount.preset ]
fi

guard=$(find /usr -type f -name nasmount-package-guard -print -quit)
boot=$(find /usr -type f -name nasmount-boot -print -quit)
[ -n "$guard" ] && [ -x "$guard" ]
[ -n "$boot" ] && [ -x "$boot" ]
for command in /usr/bin/nasmount-cleanup "$guard" "$boot"; do
    "$command" --version | grep -Fq " $expected_version"
done

# The disposable root has no managed share state, so the same gate invoked by
# prerm/%preun must authorize the subsequent package-manager smoke removal.
"$guard"
printf '%s %s installed payload passed smoke inspection.\n' "$family" "$expected_version"
