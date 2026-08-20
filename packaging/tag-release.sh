#!/bin/bash
# Create the release tag that exactly matches the checked-in VERSION.
# SPDX-License-Identifier: GPL-3.0-or-later
#
# VERSION remains the only place where a maintainer enters the application
# version. This helper fails before tagging unless the version commit is the
# clean master tip already published at origin/master. It creates a local tag
# only: keeping the push explicit gives the maintainer one final review point
# before the tag-triggered release workflow starts.

set -euo pipefail

usage()
{
    cat <<'EOF'
Usage: ./packaging/tag-release.sh [--sign]

Create a local annotated release tag from VERSION. Use --sign to create a
signed annotated tag with the Git signing key configured for this repository.
The helper validates the release state but never pushes the tag.
EOF
}

fail()
{
    printf 'ERROR: %s\n' "$*" >&2
    exit 1
}

sign_tag=false
case "${1:-}" in
    "")
        ;;
    --sign)
        sign_tag=true
        ;;
    -h|--help)
        usage
        exit 0
        ;;
    *)
        usage >&2
        exit 2
        ;;
esac
[ "$#" -le 1 ] || {
    usage >&2
    exit 2
}

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"

git rev-parse --is-inside-work-tree >/dev/null 2>&1 \
    || fail "$repo_root is not a Git worktree"
[ "$(git rev-parse --show-toplevel)" = "$repo_root" ] \
    || fail "the helper must be stored directly under the repository's packaging directory"

version_file="$repo_root/VERSION"
release_file="$repo_root/packaging/RELEASE"
[ -f "$version_file" ] || fail "VERSION is missing"
[ -f "$release_file" ] || fail "packaging/RELEASE is missing"

version="$(<"$version_file")"
release="$(<"$release_file")"
[[ "$version" =~ ^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$ ]] \
    || fail "VERSION must contain exactly MAJOR.MINOR.PATCH"
[[ "$release" =~ ^[1-9][0-9]*$ ]] \
    || fail "packaging/RELEASE must contain a positive integer"

tag="v$version"
branch="$(git branch --show-current)"
[ "$branch" = master ] \
    || fail "release tags must be created from master, not ${branch:-a detached HEAD}"

status="$(git status --porcelain --untracked-files=all)"
[ -z "$status" ] || fail "the worktree is not clean; commit or remove all changes first"

head="$(git rev-parse HEAD)"
git remote get-url origin >/dev/null 2>&1 \
    || fail "the origin remote is not configured"

remote_master_line=""
if ! remote_master_line="$(git ls-remote --exit-code origin refs/heads/master)"; then
    fail "cannot read refs/heads/master from origin"
fi
read -r remote_head remote_ref <<<"$remote_master_line"
[ "$remote_ref" = refs/heads/master ] \
    || fail "origin did not return an unambiguous master reference"
[ "$remote_head" = "$head" ] \
    || fail "HEAD is not the published origin/master tip; push master and let CI pass first"

if git show-ref --verify --quiet "refs/tags/$tag"; then
    fail "local tag $tag already exists"
fi

if remote_tag="$(git ls-remote --exit-code --tags origin "refs/tags/$tag" 2>/dev/null)"; then
    fail "remote tag $tag already exists (${remote_tag%%[[:space:]]*})"
else
    remote_status=$?
    [ "$remote_status" -eq 2 ] \
        || fail "could not determine whether $tag already exists on origin"
fi

message="nasmount $version (package release $release)"
if "$sign_tag"; then
    git tag -s -m "$message" "$tag" "$head"
else
    git tag -a -m "$message" "$tag" "$head"
fi

signed_label=""
if "$sign_tag"; then
    signed_label="signed "
fi
printf 'Created local %srelease tag %s at %s.\n' "$signed_label" "$tag" "$head"
printf 'Review it with: git show --no-patch %s\n' "$tag"
printf 'When ready, trigger the release workflow with:\n'
printf '  git push origin refs/tags/%s\n' "$tag"
