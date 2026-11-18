#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Cezary Manista
#
# release.sh — prepare a uchttp release.
#
# It bumps the version in the top-level CMakeLists.txt (the single source of
# truth), checks that CHANGELOG.md has a matching section, and — unless run in
# dry-run mode — creates the release commit and an annotated tag.
#
# It deliberately NEVER pushes. After it finishes it prints the exact push
# commands so you stay in control of what reaches the remote.
#
# Usage:
#   scripts/release.sh <version>            # e.g. 0.2.0  (prepares commit + tag)
#   scripts/release.sh <version> --dry-run  # validate + show diff only
#   scripts/release.sh --help
#
# Version must be bare SemVer MAJOR.MINOR.PATCH (no leading 'v').

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
CMAKE_FILE="${ROOT_DIR}/CMakeLists.txt"
CHANGELOG="${ROOT_DIR}/CHANGELOG.md"

die()  { echo "error: $*" >&2; exit 1; }
info() { echo ">> $*"; }

usage() {
    sed -n '5,20p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
    exit "${1:-0}"
}

DRY_RUN=0
VERSION=""
for arg in "$@"; do
    case "$arg" in
        --dry-run) DRY_RUN=1 ;;
        -h|--help) usage 0 ;;
        -*)        die "unknown option: $arg" ;;
        *)         VERSION="$arg" ;;
    esac
done

[ -n "$VERSION" ] || usage 1

# --- validate version format ------------------------------------------------
if ! [[ "$VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
    die "version must be MAJOR.MINOR.PATCH (got '$VERSION')"
fi
TAG="v${VERSION}"

# --- must be run inside the repo on a clean tree ----------------------------
cd "$ROOT_DIR"
git rev-parse --is-inside-work-tree >/dev/null 2>&1 || die "not a git repository"

CURRENT="$(sed -nE 's/.*project\(uchttp VERSION ([0-9]+\.[0-9]+\.[0-9]+).*/\1/p' "$CMAKE_FILE")"
[ -n "$CURRENT" ] || die "could not read current version from $CMAKE_FILE"
info "current version: $CURRENT"
info "new version:     $VERSION"

# --- sanity checks ----------------------------------------------------------
if [ "$CURRENT" = "$VERSION" ]; then
    die "version is already $VERSION"
fi

if git rev-parse -q --verify "refs/tags/${TAG}" >/dev/null; then
    die "tag ${TAG} already exists"
fi

# Ensure the version only increases (lexicographic SemVer compare via sort -V).
HIGHEST="$(printf '%s\n%s\n' "$CURRENT" "$VERSION" | sort -V | tail -n1)"
[ "$HIGHEST" = "$VERSION" ] || die "new version $VERSION is not greater than current $CURRENT"

if [ "$DRY_RUN" -eq 0 ] && [ -n "$(git status --porcelain)" ]; then
    die "working tree is not clean; commit or stash changes first"
fi

# CHANGELOG must document this release.
if ! grep -qE "^## \[${VERSION}\]" "$CHANGELOG"; then
    cat >&2 <<EOF
error: CHANGELOG.md has no section for ${VERSION}.
       Move the [Unreleased] entries under a new heading:

         ## [${VERSION}] - $(date +%F)

       and update the comparison links at the bottom, then re-run.
EOF
    exit 1
fi

# --- apply version bump to CMakeLists.txt -----------------------------------
info "updating $CMAKE_FILE"
BACKUP="$(mktemp)"
cp "$CMAKE_FILE" "$BACKUP"
# Restore the original file if we exit unexpectedly before committing.
cleanup() { [ -n "${BACKUP:-}" ] && [ -f "$BACKUP" ] && rm -f "$BACKUP"; }
trap cleanup EXIT

sed -i -E "s/(project\(uchttp VERSION )[0-9]+\.[0-9]+\.[0-9]+/\1${VERSION}/" "$CMAKE_FILE"

NEW="$(sed -nE 's/.*project\(uchttp VERSION ([0-9]+\.[0-9]+\.[0-9]+).*/\1/p' "$CMAKE_FILE")"
if [ "$NEW" != "$VERSION" ]; then
    cp "$BACKUP" "$CMAKE_FILE"
    die "failed to update version in $CMAKE_FILE"
fi

echo
info "diff:"
# Only the version line should differ from the backup.
diff -u "$BACKUP" "$CMAKE_FILE" || true
echo

if [ "$DRY_RUN" -eq 1 ]; then
    info "dry-run: restoring $CMAKE_FILE"
    cp "$BACKUP" "$CMAKE_FILE"
    info "dry-run complete — no commit, no tag created."
    exit 0
fi

# --- commit + annotated tag (no push) ---------------------------------------
info "creating release commit and annotated tag ${TAG}"
git add "$CMAKE_FILE" "$CHANGELOG"
git commit -m "release: ${TAG}"
git tag -a "${TAG}" -m "uchttp ${TAG}"

cat <<EOF

Done. Nothing has been pushed.

Review with:
    git show ${TAG}

Then push the release (this triggers the Release workflow):
    git push origin master
    git push origin ${TAG}

To undo before pushing:
    git tag -d ${TAG}
    git reset --hard HEAD~1
EOF
