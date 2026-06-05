#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${PROJECT_ROOT}/build"
NPROC="$(nproc)"

# Only reconfigure when not already a Ninja build.
# Wipe the directory if it exists but has no build.ninja (stale or wrong generator).
if [[ ! -f "${BUILD_DIR}/build.ninja" ]]; then
    rm -rf "${BUILD_DIR}"
    cmake -S "$PROJECT_ROOT" -B "$BUILD_DIR" \
        -G Ninja \
        -DCMAKE_BUILD_TYPE=Debug
fi

cmake --build "$BUILD_DIR" --target msga --parallel "$NPROC"
