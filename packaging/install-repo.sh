#!/usr/bin/env bash
# LiteMon one-command repository setup.
#
# Detects the package manager (APT or DNF/YUM), installs the repository
# signing key into /etc/apt/keyrings (deb822 Signed-By) or references it
# from /etc/yum.repos.d, and enables the LiteMon package repository so
# `apt install litemon` / `dnf install litemon` works like an official
# distribution package.
#
# Usage:
#   curl -fsSL https://<packages-host>/install.sh | sudo bash
#   sudo ./install-repo.sh [repo-base-url]
#
# The repository base URL defaults to
#   https://packages.litemon.dev
# and can be overridden by the first argument or $LITEMON_REPO_URL.

set -euo pipefail

REPO_URL="${1:-${LITEMON_REPO_URL:-https://packages.litemon.dev}}"
KEY_URL="${REPO_URL}/key.gpg"

need_root() {
    if [ "$(id -u)" -ne 0 ]; then
        echo "Please run as root (e.g. curl -fsSL ${REPO_URL}/install.sh | sudo bash)" >&2
        exit 1
    fi
}

if command -v apt-get >/dev/null 2>&1; then
    need_root
    echo "==> Configuring APT repository: ${REPO_URL}/apt"
    install -d -m 0755 /etc/apt/keyrings
    curl -fsSL "$KEY_URL" -o /etc/apt/keyrings/litemon.asc
    chmod 0644 /etc/apt/keyrings/litemon.asc
    # deb822 format: current Debian/Ubuntu recommendation, with the key
    # referenced by Signed-By instead of the deprecated apt-key.
    suite="$(. /etc/os-release && echo "${VERSION_CODENAME:-stable}")"
    cat > /etc/apt/sources.list.d/litemon.sources <<EOF
Types: deb
URIs: ${REPO_URL}/apt
Suites: ${suite}
Components: main
Signed-By: /etc/apt/keyrings/litemon.asc
EOF
    apt-get update
    echo "==> Done. Install with: sudo apt install litemon litemon-collector"

elif command -v dnf >/dev/null 2>&1 || command -v yum >/dev/null 2>&1; then
    need_root
    echo "==> Configuring DNF/YUM repository: ${REPO_URL}/rpm/\$basearch"
    cat > /etc/yum.repos.d/litemon.repo <<EOF
[litemon]
name=LiteMon Repository
baseurl=${REPO_URL}/rpm/\$basearch
enabled=1
gpgcheck=1
repo_gpgcheck=0
gpgkey=${KEY_URL}
EOF
    if command -v dnf >/dev/null 2>&1; then dnf makecache; else yum makecache; fi
    echo "==> Done. Install with: sudo dnf install litemon"

else
    echo "Unsupported system: no apt-get or dnf/yum found." >&2
    exit 1
fi
