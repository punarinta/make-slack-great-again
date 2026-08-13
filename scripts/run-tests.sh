#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$SCRIPT_DIR/.."
BUILD_DIR="$ROOT/build-tests"
NPROC="$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)"

source "${SCRIPT_DIR}/qt-prefix.sh"
msga_resolve_qt_prefix
msga_qt_cmake_args
msga_qt_report

if [[ ! -f "${BUILD_DIR}/build.ninja" ]] || msga_qt_prefix_changed "$BUILD_DIR"; then
    rm -rf "${BUILD_DIR}"
    cmake -S "$ROOT" -B "$BUILD_DIR" \
        -G Ninja \
        -DCMAKE_BUILD_TYPE=Debug \
        -DMSGA_BUILD_TESTS=ON \
        "${MSGA_QT_CMAKE_ARGS[@]+"${MSGA_QT_CMAKE_ARGS[@]}"}"
fi

cmake --build "$BUILD_DIR" --parallel "$NPROC"

cd "$BUILD_DIR"
ctest --output-on-failure
