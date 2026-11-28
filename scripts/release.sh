#!/usr/bin/env bash
# Release-flags build of msga for this host → build-release/msga (macOS:
# build-release/msga.app) with the per-module size report: what the size work
# measures. Prefers clang (the toolchain the releases ship with) when CC/CXX
# aren't set. Windows: scripts\release.ps1.
#
# The shipped artifacts come from the release scripts, into dist/:
#   scripts/release-linux-static.sh   static musl binary (Docker)
#   scripts/release-windows.sh        static .exe, cross-compiled (Docker)
#   scripts/release-mac.sh            DMG, on a Mac (release-mac-remote.sh from here)
#   scripts/publish-release.sh        uploads dist/
# Usage:
#   scripts/release.sh         # MinSizeRel, stripped, plus the size report
#
# Always a clean configure of the release flags: no tests, no demo workspace
# (the fake backend must never ship), whatever an earlier build dir had cached.
set -euo pipefail

case "$(uname -s)" in
    MINGW* | MSYS* | CYGWIN*)
        echo "on Windows use scripts\\release.ps1 (PowerShell)" >&2
        exit 2
        ;;
esac

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${PROJECT_ROOT}/build-release"
NPROC="$(nproc 2>/dev/null || sysctl -n hw.ncpu)"

if [[ $# -gt 0 ]]; then
    echo "unknown argument: $1" >&2
    exit 2
fi

# Wipe a directory that exists without build.ninja (stale or wrong generator).
if [[ -d "$BUILD_DIR" && ! -f "${BUILD_DIR}/build.ninja" ]]; then
    rm -rf "$BUILD_DIR"
fi

# Reconfigure every time: MSGA_DEMO / MSGA_BUILD_TESTS are cached options, so a
# hand-run `cmake -DMSGA_DEMO=ON build-release` would otherwise stick.
# Explicit compilers: CMake starts the cache over when they change.
COMPILERS=()
if [[ -z "${CC:-}${CXX:-}" ]] && command -v clang >/dev/null 2>&1 && command -v clang++ >/dev/null 2>&1; then
    COMPILERS=(-DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++)
fi
cmake -S "$PROJECT_ROOT" -B "$BUILD_DIR" -G Ninja "${COMPILERS[@]+"${COMPILERS[@]}"}" \
    -DCMAKE_BUILD_TYPE=MinSizeRel \
    -DMSGA_DEMO=OFF \
    -DMSGA_BUILD_TESTS=OFF \
    -DMSGA_SIZE_MAP=ON

cmake --build "$BUILD_DIR" --target msga --parallel "$NPROC"

# macOS builds the bundle, msga.app (bundle id com.nisdos.msga).
BIN="${BUILD_DIR}/msga"
if [[ "$(uname -s)" == "Darwin" ]]; then
    BIN="${BUILD_DIR}/msga.app/Contents/MacOS/msga"
fi

# lld already links with -s on Linux; the Apple linker (and a Linux build
# without lld) doesn't, so strip here. Idempotent on an already-stripped file.
strip "$BIN"

echo ""
if [[ -f "${BUILD_DIR}/msga.map" ]]; then
    python3 "${PROJECT_ROOT}/src/tools/size_report.py" "${BUILD_DIR}/msga.map"
    echo ""
fi
echo "Binary:  ${BIN}"
ls -lh "$BIN"
