#!/bin/bash
#
# Persistent native-package uninstall entry point.
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This command must run as the owning desktop user: nasmount-cleanup obtains
# KAuth authorization and purges verified share state before apt/dnf is
# allowed to remove the helper and policy that make that purge possible.

set -euo pipefail

readonly NASMOUNT_MANIFEST=/usr/share/nasmount/cleanup-manifest.txt
readonly NASMOUNT_FAMILY_FILE=/usr/share/nasmount/package-family
readonly NASMOUNT_CLEANUP=/usr/bin/nasmount-cleanup

package_manager_binary()
{
    case "$1" in
        deb) printf '%s\n' /usr/bin/apt-get ;;
        rpm) printf '%s\n' /usr/bin/dnf ;;
        *) return 1 ;;
    esac
}

package_manager_remove_arguments()
{
    case "$1" in
        deb) printf '%s\n' remove nasmount ;;
        # DNF may continue auto-removing dependencies after an RPM %preun
        # guard refuses the transaction. Keeping dependencies makes the
        # supported uninstall path atomic with respect to installed packages.
        rpm) printf '%s\n' remove --no-autoremove nasmount ;;
        *) return 1 ;;
    esac
}

validate_root_file()
{
    local path=$1
    local maximum_size=$2
    local owner mode size

    [ -f "$path" ] && [ ! -L "$path" ] || {
        echo "ERROR: required package metadata is not a regular file: $path" >&2
        return 1
    }
    owner=$(stat -c '%u' -- "$path")
    mode=$(stat -c '%a' -- "$path")
    size=$(stat -c '%s' -- "$path")
    [ "$owner" = 0 ] || {
        echo "ERROR: package metadata is not owned by root: $path" >&2
        return 1
    }
    (( (8#$mode & 8#022) == 0 )) || {
        echo "ERROR: package metadata is group/world writable: $path" >&2
        return 1
    }
    [ "$size" -le "$maximum_size" ] || {
        echo "ERROR: package metadata is unexpectedly large: $path" >&2
        return 1
    }
}

read_package_family()
{
    local family extra
    IFS= read -r family < "$NASMOUNT_FAMILY_FILE"
    IFS= read -r extra < <(sed -n '2p' "$NASMOUNT_FAMILY_FILE") || true
    [ -z "$extra" ] || {
        echo "ERROR: package-family metadata contains extra lines." >&2
        return 1
    }
    case "$family" in
        source|deb|rpm) printf '%s\n' "$family" ;;
        *)
            echo "ERROR: unsupported package family '$family'." >&2
            return 1
            ;;
    esac
}

remove_native_package()
{
    local family=$1
    local manager
    local argument_lines
    local -a arguments
    manager=$(package_manager_binary "$family") || {
        echo "ERROR: no native package manager for '$family'." >&2
        return 1
    }
    argument_lines=$(package_manager_remove_arguments "$family") || {
        echo "ERROR: no native package removal arguments for '$family'." >&2
        return 1
    }
    mapfile -t arguments <<< "$argument_lines"
    [ -x /usr/bin/sudo ] || {
        echo "ERROR: /usr/bin/sudo is required to remove the package." >&2
        return 1
    }
    [ -x "$manager" ] || {
        echo "ERROR: expected package manager is not installed: $manager" >&2
        return 1
    }

    /usr/bin/sudo -- "$manager" "${arguments[@]}"
}

main()
{
    if [ "$(id -u)" -eq 0 ]; then
        echo "Do not run nasmount-uninstall as root." >&2
        return 1
    fi

    validate_root_file "$NASMOUNT_MANIFEST" $((64 * 1024))
    validate_root_file "$NASMOUNT_FAMILY_FILE" 32
    local family
    family=$(read_package_family)
    if [ "$family" = source ]; then
        echo "This installation is owned by a source checkout." >&2
        echo "Run ./uninstall.sh from the checkout that installed nasmount." >&2
        return 1
    fi
    [ -x "$NASMOUNT_CLEANUP" ] || {
        echo "ERROR: $NASMOUNT_CLEANUP is not installed." >&2
        return 1
    }

    echo "This will purge every nasmount share owned by your user, remove"
    echo "your nasmount configuration, and then remove the native package."
    echo "Mount-point directories will remain."
    read -r -p "Proceed? [y/N] " answer
    [ "$answer" = y ] || [ "$answer" = Y ] || {
        echo "Aborted."
        return 0
    }

    "$NASMOUNT_CLEANUP" --manifest "$NASMOUNT_MANIFEST"
    remove_native_package "$family"

    if command -v kbuildsycoca6 >/dev/null 2>&1; then
        kbuildsycoca6 --noincremental >/dev/null 2>&1 || true
    else
        echo "Restart Dolphin and System Settings to refresh KDE integration."
    fi
}

if [[ "${BASH_SOURCE[0]}" == "$0" ]]; then
    main "$@"
fi
