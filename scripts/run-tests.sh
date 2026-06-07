#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$SCRIPT_DIR/.."
BUILD_DIR="$ROOT/build-tests"
NPROC="$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)"

CMAKE_EXTRA_ARGS=()
if [[ -n "${QT_PREFIX:-}" ]]; then
    CMAKE_EXTRA_ARGS+=("-DCMAKE_PREFIX_PATH=${QT_PREFIX}")
fi

if [[ ! -f "${BUILD_DIR}/build.ninja" ]]; then
    rm -rf "${BUILD_DIR}"
    cmake -S "$ROOT" -B "$BUILD_DIR" \
        -G Ninja \
        -DCMAKE_BUILD_TYPE=Debug \
        -DMSGA_BUILD_TESTS=ON \
        "${CMAKE_EXTRA_ARGS[@]}"
fi

cmake --build "$BUILD_DIR" --parallel "$NPROC"

cd "$BUILD_DIR"
ctest --output-on-failure
