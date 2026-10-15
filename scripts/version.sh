#!/usr/bin/env bash
# Single source of truth for the release version: VERSION at the repo root.
# CMakeLists.txt reads it, so every platform build (CI or cross-compile)
# that configures from the same commit gets the same version string.
#
# Every build consumes a FRESH number: scripts/build.sh calls `bump` before
# compiling, so no binary is ever produced twice under the same version.
#
# Usage:
#   scripts/version.sh                        # print current version
#   scripts/version.sh bump [major|minor|patch]  # bump one field (default patch), print new version
#   scripts/version.sh set X.Y.Z              # set an explicit version
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VERSION_FILE="$ROOT/VERSION"

current() { sed 's/[[:space:]]//' "$VERSION_FILE"; }

bump_one() { # X.Y.Z [major|minor|patch] -> bumped version
    local part="${2:-patch}" major minor patch
    IFS=. read -r major minor patch <<< "$1"
    case "$part" in
        major) printf '%s.0.0' "$((major + 1))" ;;
        minor) printf '%s.%s.0' "$major" "$((minor + 1))" ;;
        patch) printf '%s.%s.%s' "$major" "$minor" "$((patch + 1))" ;;
    esac
}

cmd="${1:-current}"
case "$cmd" in
    current)
        current
        ;;
    bump)
        new="$(bump_one "$(current)" "${2:-patch}")"
        printf '%s\n' "$new" > "$VERSION_FILE"
        echo "$new"
        ;;
    set)
        [ -n "${2:-}" ] || { echo "usage: version.sh set X.Y.Z" >&2; exit 2; }
        if ! printf '%s' "$2" | grep -Eq '^[0-9]+\.[0-9]+\.[0-9]+$'; then
            echo "VERSION must be X.Y.Z, got: $2" >&2
            exit 2
        fi
        printf '%s\n' "$2" > "$VERSION_FILE"
        echo "$2"
        ;;
    *)
        echo "usage: version.sh [current|bump|set X.Y.Z]" >&2
        exit 2
        ;;
esac
