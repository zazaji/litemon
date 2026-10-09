#!/usr/bin/env bash
# LiteMon cross-compile build for macOS arm64 + Windows x64.
# Runs ON home113 (Debian CT with osxcross + mingw-w64 + aqt Qt 6.8.2).
# Usage: ssh home113 'bash -s <src-dir> <out-dir>' < scripts/home113-build.sh
# Environment rules: /opt/osxcross, /opt/qt6 etc. must never be deleted or cleaned.
set -euo pipefail

SRC="${1:?usage: bash -s <src-dir> <out-dir> (via ssh home113)}"
OUT="${2:?missing out-dir}"
V="$(sed 's/[[:space:]]//' "$SRC/VERSION")"
QT_MAC=/opt/qt6/6.8.2/macos
QT_WIN=/opt/qt6/6.8.2/mingw_64
OSX_BIN=/opt/osxcross/target/bin
mkdir -p "$OUT"
cd "$SRC"

# ---------------------------------------------------------------- macOS arm64
echo "== mac arm64: configure =="
export PATH="$OSX_BIN:$PATH"   # linker resolves <triple>-ld via PATH
rm -rf build-mac
arm64-apple-darwin23-cmake -S "$SRC" -B build-mac -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_FIND_ROOT_PATH="$QT_MAC" \
  -DQT_HOST_PATH=/usr \
  -DCMAKE_AUTOMOC_EXECUTABLE=/usr/lib/qt6/libexec/moc \
  -DCMAKE_SYSTEM_VERSION=14.0 \
  -DCMAKE_INSTALL_RPATH:PATH="@executable_path/../Frameworks" \
  -DCMAKE_BUILD_WITH_INSTALL_RPATH:BOOL=ON
cmake --build build-mac -j"$(nproc)"

# untyped -D silently lands as UNINITIALIZED in CMakeCache -> no LC_RPATH -> SIGKILL on Apple Silicon
for b in litemon litemon-collector; do
    file "build-mac/$b" | grep -qi Mach-O
    "$OSX_BIN/arm64-apple-darwin23-otool" -l "build-mac/$b" | grep -q LC_RPATH
done

echo "== mac arm64: assemble LiteMon.app =="
APP="$SRC/LiteMon.app"
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Frameworks" \
         "$APP/Contents/PlugIns/platforms" "$APP/Contents/Resources"
cp build-mac/litemon build-mac/litemon-collector "$APP/Contents/MacOS/"
sed "s/@LITEMON_VERSION@/$V/" "$SRC/packaging/macos/Info.plist" > "$APP/Contents/Info.plist"
printf 'APPL????\n' > "$APP/Contents/PkgInfo"

# copy exactly the Qt frameworks the built binaries reference (Core, Gui,
# Widgets, Sql, DBus, Network, ...), trim Headers to keep the zip small
"$OSX_BIN/arm64-apple-darwin23-otool" -L build-mac/litemon build-mac/litemon-collector \
    | grep -o '@rpath/Qt[A-Za-z]*\.framework' | sort -u | sed 's|@rpath/||;s|\.framework||' \
    | while read -r fw; do
        cp -a "$QT_MAC/lib/$fw.framework" "$APP/Contents/Frameworks/"
        rm -rf "$APP/Contents/Frameworks/$fw.framework/Versions/A/Headers"
        mkdir -p "$APP/Contents/Frameworks/$fw.framework/Versions/A/Headers"
        : > "$APP/Contents/Frameworks/$fw.framework/Versions/A/Headers/Core_fake_header.h"
        echo "  + $fw.framework"
    done
cp "$QT_MAC/plugins/platforms/libqcocoa.dylib" "$APP/Contents/PlugIns/platforms/"

( cd "$SRC" && zip -qry "$OUT/litemon-$V-macOS.zip" LiteMon.app )
rm -rf "$APP"

# ---------------------------------------------------------------- Windows x64
echo "== win64: configure =="
rm -rf build-win
cmake -S "$SRC" -B build-win -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE=/opt/litemon-win64-toolchain.cmake \
  -DQT_HOST_PATH=/usr \
  -DCMAKE_AUTOMOC_EXECUTABLE=/usr/lib/qt6/libexec/moc \
  -DCMAKE_DISABLE_FIND_PACKAGE_WrapVulkanHeaders=TRUE
cmake --build build-win -j"$(nproc)"
for b in litemon.exe litemon-collector.exe; do
    file "build-win/$b" | grep -qi 'PE32+' || { echo "FATAL: $b is not a PE binary" >&2; exit 1; }
done

echo "== win64: assemble portable zip =="
WIN_STAGE="$SRC/litemon-win64"
rm -rf "$WIN_STAGE"
mkdir -p "$WIN_STAGE/platforms" "$WIN_STAGE/sqldrivers"
cp build-win/litemon.exe build-win/litemon-collector.exe "$WIN_STAGE/"

x86_64-w64-mingw32-objdump -p build-win/litemon.exe build-win/litemon-collector.exe \
    | awk '/DLL Name:/ {print $3}' | sort -u \
    | while read -r dll; do
        [ -f "$QT_WIN/bin/$dll" ] || continue   # skip system DLLs (KERNEL32, ...)
        cp "$QT_WIN/bin/$dll" "$WIN_STAGE/"
        echo "  + $dll"
    done
for rt in libgcc_s_seh-1.dll libstdc++-6.dll libwinpthread-1.dll; do
    cp "$QT_WIN/bin/$rt" "$WIN_STAGE/"
done
cp "$QT_WIN/plugins/platforms/qwindows.dll" "$WIN_STAGE/platforms/"
cp "$QT_WIN/plugins/sqldrivers/qsqlite.dll" "$WIN_STAGE/sqldrivers/"
( cd "$SRC" && zip -qry "$OUT/litemon-$V-win64.zip" "$(basename "$WIN_STAGE")" )
rm -rf "$WIN_STAGE"

echo "== done: $(ls -la "$OUT")"
