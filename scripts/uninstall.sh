#!/usr/bin/env bash
set -euo pipefail
PREFIX="${HOME}/.local"
systemctl --user disable --now litemon-collector.service >/dev/null 2>&1 || true
rm -f "$HOME/.config/systemd/user/litemon-collector.service"
systemctl --user daemon-reload
rm -f "$PREFIX/bin/litemon" "$PREFIX/bin/litemon-collector"
rm -f "$PREFIX/share/applications/io.github.litemon.LiteMon.desktop"
rm -f "$PREFIX/share/metainfo/io.github.litemon.LiteMon.metainfo.xml"
rm -f "$PREFIX/share/icons/hicolor/scalable/apps/io.github.litemon.LiteMon.svg"
echo "LiteMon binaries and service removed. User data/config remain in XDG litemon directories."
