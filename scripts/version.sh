#!/usr/bin/env bash
# Single source of truth for the release version: VERSION at the repo root.
# CMakeLists.txt reads it, so every platform build (CI or cross-compile)
# that configures from the same commit gets the same version string.
#
# Every build consumes a FRESH number: scripts/build.sh calls `bump` before
# compiling, so no binary is ever produced twice under the same version.
#
# Usage:
#   scripts/version.sh            # print current version
#   scripts/version.sh bump       # X.Y.Z -> X.Y.(Z+1), print new version
#   scripts/version.sh set X.Y.Z  # set an explicit version
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VERSION_FILE="$ROOT/VERSION"

current() { sed 's/[[:space:]]//' "$VERSION_FILE"; }

bump_one() { # X.Y.Z -> X.Y.(Z+1)
    local major minor patch
    IFS=. read -r major minor patch <<< "$1"
    printf '%s.%s.%s' "$major" "$minor" "$((patch + 1))"
}

cmd="${1:-current}"
case "$cmd" in
    current)
        current
        ;;
    bump)
        new="$(bump_one "$(current)")"
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
