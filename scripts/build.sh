#!/usr/bin/env bash
# Build msga (the root CMake project: src/ on src/plat) on Linux, macOS,
# FreeBSD or OpenBSD (after scripts/configure-bsd.sh).
# Windows: scripts\build.ps1.
# Usage:
#   scripts/build.sh           # MinSizeRel → build/msga
#   scripts/build.sh --debug   # Debug      → build-debug/msga
#   scripts/build.sh --test    # also build and run the test suite (combines with --debug)
#   scripts/build.sh --demo    # also compile the demo workspace (msga --demo demo)
#   scripts/build.sh --asan    # AddressSanitizer + LeakSanitizer → build-asan/msga
#                              #   (run it with scripts/run-asan.sh)
# --test and --demo switch the option on in that build dir; it stays on there.
set -euo pipefail

case "$(uname -s)" in
    MINGW* | MSYS* | CYGWIN*)
        echo "on Windows use scripts\\build.ps1 (PowerShell)" >&2
        exit 2
        ;;
esac

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
NPROC="$(nproc 2>/dev/null || sysctl -n hw.ncpu)"

DEBUG=0
ASAN=0
TEST=0
OPTIONS=()
for arg in "$@"; do
    case "$arg" in
        --debug) DEBUG=1 ;;
        --asan) ASAN=1 ;;
        --test) TEST=1; OPTIONS+=(-DMSGA_BUILD_TESTS=ON) ;;
        --demo) OPTIONS+=(-DMSGA_DEMO=ON) ;;
        *) echo "unknown argument: $arg" >&2; exit 2 ;;
    esac
done

# Size is the default build: the release flags are what the size budgets
# measure. Debug and ASan get their own dirs so none share objects.
if [[ "$ASAN" == "1" ]]; then
    BUILD_DIR="${PROJECT_ROOT}/build-asan"
    BUILD_TYPE=Debug
elif [[ "$DEBUG" == "1" ]]; then
    BUILD_DIR="${PROJECT_ROOT}/build-debug"
    BUILD_TYPE=Debug
else
    BUILD_DIR="${PROJECT_ROOT}/build"
    BUILD_TYPE=MinSizeRel
fi

# Only configure when not already a Ninja build. Wipe a directory that exists
# without build.ninja (stale or wrong generator).
if [[ ! -f "${BUILD_DIR}/build.ninja" ]]; then
    rm -rf "${BUILD_DIR}"
    cmake -S "$PROJECT_ROOT" -B "$BUILD_DIR" -G Ninja -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
        -DMSGA_ASAN="$([[ "$ASAN" == "1" ]] && echo ON || echo OFF)" \
        "${OPTIONS[@]+"${OPTIONS[@]}"}"
elif [[ ${#OPTIONS[@]} -gt 0 ]]; then
    cmake "$BUILD_DIR" "${OPTIONS[@]}"
fi

cmake --build "$BUILD_DIR" --parallel "$NPROC"

if [[ "$TEST" == "1" ]]; then
    ctest --test-dir "$BUILD_DIR" --output-on-failure -j "$NPROC"
fi

echo "Built: ${BUILD_DIR}/msga"
