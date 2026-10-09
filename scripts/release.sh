#!/usr/bin/env bash
# LiteMon release pipeline — single entry point for a tagged release.
#
# Flow (everything automated, one command):
#   1. prechecks: GitHub auth, clean tree, master synced with origin
#   2. bump VERSION (fresh number for every release, shared by all platforms)
#   3. local build + full test suite (release preset)
#   4. cross-compile macOS arm64 + Windows x64 on home113 (ssh), fetch the
#      zips into build/dist/
#   5. commit VERSION + metainfo, tag v<version>, push master + tag
#   6. wait for the tag-triggered Release workflow: GitHub Actions builds
#      linux .deb/.tar.gz/.rpm from the SAME tag and the SAME VERSION file
#   7. upload the home113-built macOS/Windows zips to the same release
#   8. verify the release and its assets on GitHub Releases
#
# Usage:
#   scripts/release.sh                # full release (patch bump + tag + push)
#   scripts/release.sh --skip-build   # skip local build/test before tagging
#   scripts/release.sh --dry-run      # precheck + bump + build only, no push
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
REPO="zazaji/litemon"
REMOTE_HOST="home113"
REMOTE_STAGE="/tmp/litemon-release"

DRY=0; SKIP_BUILD=0
for a in "$@"; do
    case "$a" in
        --dry-run) DRY=1 ;;
        --skip-build) SKIP_BUILD=1 ;;
        *) echo "unknown arg: $a (supported: --dry-run --skip-build)" >&2; exit 2 ;;
    esac
done

say() { printf '\033[1;36m== %s\033[0m\n' "$*"; }

# --- 1. GitHub auth: gh reads GITHUB_TOKEN from the environment -------------
# shellcheck disable=SC1090
eval "$(grep -m1 '^export GITHUB_TOKEN=' "$HOME/.bashrc")"
gh auth status >/dev/null || {
    echo "GitHub auth failed — see ~/.zcode/skills/github-push-auth/SKILL.md" >&2
    exit 1
}

# 2. prechecks ----------------------------------------------------------------
if [ -n "$(git status --porcelain --untracked-files=no)" ]; then
    echo "Uncommitted tracked changes — commit them first:" >&2
    git status --short --untracked-files=no >&2
    exit 1
fi
git fetch origin master --quiet
[ "$(git rev-parse HEAD)" = "$(git rev-parse origin/master)" ] || {
    echo "master diverged from origin/master — pull/rebase first" >&2
    exit 1
}

OLD_VERSION="$(scripts/version.sh current)"
NEW_VERSION="$(scripts/version.sh bump)"
echo "version: ${OLD_VERSION} -> ${NEW_VERSION}"
TAG="v${NEW_VERSION}"

# appstream <release> line follows VERSION so metainfo is never stale
TODAY="$(date +%F)"
sed -i "s|<releases>.*</releases>|<releases><release version=\"${NEW_VERSION}\" date=\"${TODAY}\"/></releases>|" \
    packaging/io.github.litemon.LiteMon.metainfo.xml

# 3. local build + tests (tag pins this version; no second bump)
if [ "${SKIP_BUILD:-0}" != "1" ]; then
    LITEMON_NO_AUTOBUMP=1 scripts/build.sh release
fi

# 4. cross-compile macOS + Windows on home113, fetch the zips -----------------
DIST="$ROOT/build/dist"
mkdir -p "$DIST"
say "cross-compiling mac/win on ${REMOTE_HOST} ..."
# stage path is a constant; expanding client-side is intended
# shellcheck disable=SC2029
ssh "$REMOTE_HOST" "rm -rf ${REMOTE_STAGE} && mkdir -p ${REMOTE_STAGE}/out"
rsync -a --delete --exclude .git --exclude build --exclude 'LiteMon.app' \
    --exclude 'litemon-win64' "$ROOT/" "${REMOTE_HOST}:${REMOTE_STAGE}/src/"
# shellcheck disable=SC2029
ssh "$REMOTE_HOST" "bash -s ${REMOTE_STAGE}/src ${REMOTE_STAGE}/out" \
    < "$ROOT/scripts/home113-build.sh"
rsync -a "${REMOTE_HOST}:${REMOTE_STAGE}/out/" "$DIST/"
ls -la "$DIST"

if [ "$DRY" = "1" ]; then
    echo "-- dry run: would commit 'release: v${NEW_VERSION}', tag and push --"
    exit 0
fi

# 5. commit, tag, push --------------------------------------------------------
git add VERSION packaging/io.github.litemon.LiteMon.metainfo.xml
git commit -m "release: v${NEW_VERSION}"
git tag -a "${TAG}" -m "LiteMon ${NEW_VERSION}"
git push origin master "${TAG}"

# 6. wait for the Release workflow triggered by the tag (Linux packages) ------
say "watching Release workflow for tag ${TAG} ..."
sleep 10
RUN_ID=""
for _ in $(seq 1 30); do
    RUN_ID="$(gh run list --repo "$REPO" --workflow=release.yml --limit 10 \
        --json headBranch,databaseId \
        --jq ".[] | select(.headBranch==\"${TAG}\") | .databaseId" | head -1)"
    [ -n "$RUN_ID" ] && break
    sleep 10
done
if [ -z "$RUN_ID" ]; then
    echo "Release workflow did not start for ${TAG}" >&2
    exit 1
fi
gh run watch "$RUN_ID" --repo "$REPO" --exit-status

# 7. attach the home113-built artifacts to the same release -------------------
say "uploading mac/win artifacts to ${TAG} ..."
gh release upload "$TAG" --repo "$REPO" "$DIST"/litemon-"${NEW_VERSION}"-macOS.zip \
                                             "$DIST"/litemon-"${NEW_VERSION}"-win64.zip --clobber

# 8. verify release assets (3 platforms, same version) ------------------------
say "release ${TAG} created — assets:"
gh release view "$TAG" --repo "$REPO" --json assets --jq '.assets[].name'
echo "release URL: https://github.com/${REPO}/releases/tag/${TAG}"
