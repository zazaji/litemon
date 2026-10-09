#!/usr/bin/env bash
# Build + test. Usage: scripts/build.sh [dev|release|asan|ubsan]
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PRESET="${1:-release}"

# Rule: every rebuild ships a new version number. `bump` increments the
# patch component of VERSION before compiling; CI and tag builds pass
# LITEMON_NO_AUTOBUMP=1 (the tag already pins the version).
if [ "${LITEMON_NO_AUTOBUMP:-0}" != "1" ] && [ "${CI:-}" != "true" ]; then
    NEW_VERSION="$("$ROOT/scripts/version.sh" bump)"
    echo "== LiteMon version bumped to ${NEW_VERSION} =="
fi
echo "== LiteMon v$("$ROOT/scripts/version.sh" current) / $PRESET build =="

cmake --preset "$PRESET"
cmake --build --preset "$PRESET" -j"$(nproc)"
ctest --test-dir "$ROOT/build/$PRESET" --output-on-failure
