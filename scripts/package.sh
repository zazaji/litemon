#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
"$ROOT/scripts/build.sh" release
cd "$ROOT/build/release"
cpack -G TGZ
if command -v dpkg-shlibdeps >/dev/null && command -v fakeroot >/dev/null; then
  cpack -G DEB
else
  echo "Skipping .deb: install dpkg-dev and fakeroot to enable Debian package generation." >&2
fi
