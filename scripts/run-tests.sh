#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$SCRIPT_DIR/.."
BUILD_DIR="$ROOT/build-tests"
NPROC="$(nproc)"

if [[ ! -f "${BUILD_DIR}/build.ninja" ]]; then
    rm -rf "${BUILD_DIR}"
    cmake -S "$ROOT" -B "$BUILD_DIR" \
        -G Ninja \
        -DCMAKE_BUILD_TYPE=Debug \
        -DMSGA_BUILD_TESTS=ON
fi

cmake --build "$BUILD_DIR" --parallel "$NPROC"

cd "$BUILD_DIR"
ctest --output-on-failure
