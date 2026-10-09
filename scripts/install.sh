#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PREFIX="${HOME}/.local"
BUILD="${ROOT}/build/release"

for tool in cmake g++ ninja; do
  command -v "$tool" >/dev/null || { echo "Missing $tool. Debian 13: sudo apt install build-essential cmake ninja-build" >&2; exit 1; }
done

cmake -S "$ROOT" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build "$BUILD" -j"$(nproc)"
ctest --test-dir "$BUILD" --output-on-failure

install -Dm755 "$BUILD/litemon" "$PREFIX/bin/litemon"
install -Dm755 "$BUILD/litemon-collector" "$PREFIX/bin/litemon-collector"
install -Dm644 "$ROOT/packaging/io.github.litemon.LiteMon.svg" "$PREFIX/share/icons/hicolor/scalable/apps/io.github.litemon.LiteMon.svg"
install -Dm644 "$ROOT/packaging/io.github.litemon.LiteMon.metainfo.xml" "$PREFIX/share/metainfo/io.github.litemon.LiteMon.metainfo.xml"
mkdir -p "$PREFIX/share/applications"
sed "s#^Exec=.*#Exec=$PREFIX/bin/litemon#; s#^Icon=.*#Icon=io.github.litemon.LiteMon#" \
  "$ROOT/packaging/io.github.litemon.LiteMon.desktop" > "$PREFIX/share/applications/io.github.litemon.LiteMon.desktop"
chmod 0644 "$PREFIX/share/applications/io.github.litemon.LiteMon.desktop"
install -Dm644 "$ROOT/packaging/litemon-collector.service" "$HOME/.config/systemd/user/litemon-collector.service"
mkdir -p "$HOME/.local/share/litemon" "$HOME/.cache/litemon" "$HOME/.config/litemon"

systemctl --user daemon-reload
systemctl --user enable litemon-collector.service
# restart (not enable --now): a previous install may have left an older
# collector binary running, and enable --now would keep it.
systemctl --user restart litemon-collector.service

if command -v update-desktop-database >/dev/null; then
  update-desktop-database "$PREFIX/share/applications" >/dev/null 2>&1 || true
fi
if command -v gtk-update-icon-cache >/dev/null; then
  gtk-update-icon-cache -f -t "$PREFIX/share/icons/hicolor" >/dev/null 2>&1 || true
fi

echo "Installed LiteMon 2.0"
echo "UI: $PREFIX/bin/litemon"
echo "Health: $PREFIX/bin/litemon-collector --health-check"
echo "Status: systemctl --user status litemon-collector.service"
