#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${PROJECT_ROOT}/build-release"
NPROC="$(nproc)"

# Always reconfigure release builds to ensure optimisation flags are applied.
cmake -S "$PROJECT_ROOT" -B "$BUILD_DIR" \
    -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_CXX_FLAGS_RELEASE="-O3 -g -DNDEBUG" \
    -DCMAKE_INTERPROCEDURAL_OPTIMIZATION=ON

cmake --build "$BUILD_DIR" --target msga --parallel "$NPROC"

# Crash reports (src/app/crash_handler.cpp) resolve through the symbol
# table — never --strip-all. Debug info (-g) is split into a sidecar .debug
# file; .gnu_debuglink lets gdb/addr2line find it and turn crash-report
# frame addresses into file:line.
objcopy --only-keep-debug "${BUILD_DIR}/msga" "${BUILD_DIR}/msga.debug"
strip --strip-debug "${BUILD_DIR}/msga"
(cd "$BUILD_DIR" && objcopy --add-gnu-debuglink=msga.debug msga)

echo ""
echo "Binary:      ${BUILD_DIR}/msga"
echo "Debug syms:  ${BUILD_DIR}/msga.debug (keep with the release — resolves user crash reports)"
ls -lh "${BUILD_DIR}/msga" "${BUILD_DIR}/msga.debug"
