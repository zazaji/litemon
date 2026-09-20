#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PRESET="${1:-release}"
cmake --preset "$PRESET"
cmake --build --preset "$PRESET" -j"$(nproc)"
ctest --test-dir "$ROOT/build/$PRESET" --output-on-failure
