#!/usr/bin/env bash
# The macOS release, on a Mac: msga.app in a DMG → dist/msga-macos-<arch>.dmg
# (+ .manifest, + .dSYM). From Linux, scripts/release-mac-remote.sh runs this
# on the Mac build host and brings the artifacts back.
#
# Self-contained: AppKit and the system frameworks only. FreeType and HarfBuzz
# are built by msga's own CMake (src/text/font_libs.cmake), so no Homebrew
# library ends up in the bundle; Homebrew only provides cmake and ninja.
#
# Usage:
#   scripts/release-mac.sh [--skip-tests]
#
# Env: NPROC=<jobs> (default: all cores)
# Requires: macOS with the Xcode command line tools, cmake and ninja.
#
# Build trees (kept between runs, incremental): build-release-mac/,
# build-tests-mac/.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
OUT_DIR="${PROJECT_ROOT}/dist"
BUILD_DIR="${PROJECT_ROOT}/build-release-mac"
TESTS_DIR="${PROJECT_ROOT}/build-tests-mac"
NPROC="${NPROC:-$(sysctl -n hw.ncpu)}"
MACOS_MIN_VERSION="14.0" # = LSMinimumSystemVersion (app/resources/Info.plist.in)
SKIP_TESTS=0
STAGING=""
trap '[[ -n "$STAGING" ]] && rm -rf "$STAGING"' EXIT

for arg in "$@"; do
    case "$arg" in
        --skip-tests) SKIP_TESTS=1 ;;
        *) echo "error: unknown argument: $arg" >&2; exit 2 ;;
    esac
done

[[ "$(uname -s)" == "Darwin" ]] || { echo "error: run this on macOS (or release-mac-remote.sh)" >&2; exit 1; }
xcode-select -p >/dev/null 2>&1 || { echo "error: no Xcode command line tools: xcode-select --install" >&2; exit 1; }
for tool in cmake ninja; do
    command -v "$tool" >/dev/null 2>&1 || { echo "error: $tool missing: brew install $tool" >&2; exit 1; }
done
ARCH="$(uname -m)"

# A build dir configured for another source tree or another generator: start
# it over.
fresh_build_dir() {
    local cache="$1/CMakeCache.txt"
    if [[ -f "$cache" ]] &&
        ! grep -qx "CMAKE_HOME_DIRECTORY:INTERNAL=${PROJECT_ROOT}" "$cache"; then
        rm -rf "$1"
    fi
    if [[ -d "$1" && ! -f "$1/build.ninja" ]]; then
        rm -rf "$1"
    fi
}

CONFIGURE=(-G Ninja -DCMAKE_BUILD_TYPE=MinSizeRel -DMSGA_DEMO=OFF
    -DCMAKE_OSX_DEPLOYMENT_TARGET="$MACOS_MIN_VERSION")

# ── Tests ─────────────────────────────────────────────────────────────────────
if [[ $SKIP_TESTS -eq 0 ]]; then
    echo "Running the test suite..."
    fresh_build_dir "$TESTS_DIR"
    cmake -S "$PROJECT_ROOT" -B "$TESTS_DIR" "${CONFIGURE[@]}" -DMSGA_BUILD_TESTS=ON >/dev/null
    cmake --build "$TESTS_DIR" --parallel "$NPROC"
    ctest --test-dir "$TESTS_DIR" --output-on-failure -j "$NPROC"
fi

# ── Build ─────────────────────────────────────────────────────────────────────
# -g for the dSYM (it doesn't change the code); stripped below, after dsymutil
# has read the debug map.
fresh_build_dir "$BUILD_DIR"
cmake -S "$PROJECT_ROOT" -B "$BUILD_DIR" "${CONFIGURE[@]}" -DMSGA_BUILD_TESTS=OFF \
    -DCMAKE_C_FLAGS=-g -DCMAKE_CXX_FLAGS=-g -DCMAKE_OBJCXX_FLAGS=-g
cmake --build "$BUILD_DIR" --target msga --parallel "$NPROC"

APP="${BUILD_DIR}/msga.app"
BIN="${APP}/Contents/MacOS/msga"

# Nothing from Homebrew (or anywhere outside the OS) may be linked.
if otool -L "$BIN" | tail -n +2 | grep -vE '^\s*(/System/Library/|/usr/lib/)'; then
    echo "error: ${BIN} links libraries that aren't part of macOS (above)" >&2
    exit 1
fi

# ── dist/: the bundle, its symbols, the DMG ───────────────────────────────────
mkdir -p "$OUT_DIR"
DSYM="${OUT_DIR}/msga-macos-${ARCH}.dSYM"
rm -rf "$DSYM" "${OUT_DIR}/msga.app"
dsymutil "$BIN" -o "$DSYM"
cp -R "$APP" "${OUT_DIR}/msga.app"
APP="${OUT_DIR}/msga.app"
strip "${APP}/Contents/MacOS/msga"

# Ad hoc signatures (stripping broke the build's). The entitlements
# (microphone access for voice input) belong to the app's own executable only,
# so the bundle is signed a second time with them; --deep would stamp them on
# everything. The designated requirement is the bundle id alone: an ad hoc one
# is the build's cdhash, and the Keychain admits only the build that created an
# item, so every update would ask once per credential again (base/secret_mac.mm).
codesign --force --deep --sign - "$APP"
codesign --force --sign - --entitlements "${PROJECT_ROOT}/src/app/resources/msga.entitlements" \
    -r='designated => identifier "com.nisdos.msga"' "$APP"
codesign --verify --strict "$APP"

ARTIFACT="msga-macos-${ARCH}.dmg"
STAGING="$(mktemp -d)"
cp -R "$APP" "${STAGING}/msga.app"
ln -s /Applications "${STAGING}/Applications"
hdiutil create -volname "msga" -srcfolder "$STAGING" -ov -format UDZO "${OUT_DIR}/${ARTIFACT}" >/dev/null

# ── Version manifest ──────────────────────────────────────────────────────────
# The in-app updater (app/update) downloads <asset> and msga-macos-<arch>
# .manifest and rejects a download whose sha256 doesn't match.
MSGA_VERSION="$(grep -m1 'MSGA_VERSION' "${PROJECT_ROOT}/version.cmake" | grep -oE '[0-9]+')"
printf '{"version":%s,"updatedAt":"%s","sha256":"%s"}\n' "$MSGA_VERSION" \
    "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$(shasum -a 256 "${OUT_DIR}/${ARTIFACT}" | cut -d' ' -f1)" \
    >"${OUT_DIR}/msga-macos-${ARCH}.manifest"

echo ""
echo "Release: ${OUT_DIR}/${ARTIFACT} (version ${MSGA_VERSION})"
ls -l "${APP}/Contents/MacOS/msga" "${OUT_DIR}/${ARTIFACT}" "${OUT_DIR}/msga-macos-${ARCH}.manifest"
echo "Symbols: ${DSYM} (NOT for upload — archive it per release)"
