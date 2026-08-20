#!/bin/bash
# Rootless static/functional checks for native package shell entry points.
# SPDX-License-Identifier: GPL-3.0-or-later

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
uninstaller="$repo_root/packaging/nasmount-uninstall.sh"
release_tagger="$repo_root/packaging/tag-release.sh"

bash -n "$uninstaller"
bash -n "$release_tagger"
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

# Exercise the release helper against a local bare origin. No network access,
# credentials, tag push, or modification of the source repository is involved.
tag_test_root="$(mktemp -d)"
trap 'rm -rf -- "$tag_test_root"' EXIT
tag_test_repo="$tag_test_root/repository"
tag_test_origin="$tag_test_root/origin.git"
git init --bare --quiet "$tag_test_origin"
git init --quiet --initial-branch=master "$tag_test_repo"
mkdir "$tag_test_repo/packaging"
cp "$release_tagger" "$tag_test_repo/packaging/tag-release.sh"
cp "$repo_root/VERSION" "$tag_test_repo/VERSION"
cp "$repo_root/packaging/RELEASE" "$tag_test_repo/packaging/RELEASE"
git -C "$tag_test_repo" config user.name 'nasmount release test'
git -C "$tag_test_repo" config user.email 'release-test@nasmount.invalid'
git -C "$tag_test_repo" config tag.gpgSign false
git -C "$tag_test_repo" add VERSION packaging
git -C "$tag_test_repo" commit --quiet -m 'Test release state'
git -C "$tag_test_repo" remote add origin "$tag_test_origin"
git -C "$tag_test_repo" push --quiet --set-upstream origin master

touch "$tag_test_repo/untracked-change"
if "$tag_test_repo/packaging/tag-release.sh" >"$tag_test_root/dirty.out" 2>&1; then
    echo "ERROR: release helper accepted a dirty worktree" >&2
    exit 1
fi
rm -- "$tag_test_repo/untracked-change"

expected_tag="v$(tr -d '\n' < "$tag_test_repo/VERSION")"
tag_output="$("$tag_test_repo/packaging/tag-release.sh")"
[ "$(git -C "$tag_test_repo" cat-file -t "refs/tags/$expected_tag")" = tag ]
grep -Fq "git push origin refs/tags/$expected_tag" <<<"$tag_output"
[ -z "$(git --git-dir="$tag_test_origin" tag --list "$expected_tag")" ] || {
    echo "ERROR: release helper pushed the tag instead of leaving it local" >&2
    exit 1
}
if "$tag_test_repo/packaging/tag-release.sh" >"$tag_test_root/local-duplicate.out" 2>&1; then
    echo "ERROR: release helper accepted an existing local tag" >&2
    exit 1
fi

git -C "$tag_test_repo" push --quiet origin "refs/tags/$expected_tag"
git -C "$tag_test_repo" tag --delete "$expected_tag" >/dev/null
if "$tag_test_repo/packaging/tag-release.sh" >"$tag_test_root/remote-duplicate.out" 2>&1; then
    echo "ERROR: release helper accepted an existing remote tag" >&2
    exit 1
fi

echo "Native package shell checks passed."
