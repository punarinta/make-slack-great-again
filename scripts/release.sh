#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${PROJECT_ROOT}/build-release"
NPROC="$(nproc)"

# Same Qt discovery as build.sh — a distro Qt below the 6.5 floor has to be
# overridable here too (this is the script we point people at for a fast build).
source "${SCRIPT_DIR}/qt-prefix.sh"
msga_resolve_qt_prefix
msga_qt_cmake_args
msga_qt_report

# Qt6_DIR is cached, so pointing CMAKE_PREFIX_PATH at another Qt in an existing
# build dir would be ignored — wipe it instead.
if msga_qt_prefix_changed "$BUILD_DIR"; then
    echo "Qt changed since this build dir was configured — wiping ${BUILD_DIR}"
    rm -rf "$BUILD_DIR"
fi

# Always reconfigure release builds to ensure optimisation flags are applied.
cmake -S "$PROJECT_ROOT" -B "$BUILD_DIR" \
    -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_CXX_FLAGS_RELEASE="-O3 -g -DNDEBUG" \
    -DCMAKE_INTERPROCEDURAL_OPTIMIZATION=ON \
    "${MSGA_QT_CMAKE_ARGS[@]+"${MSGA_QT_CMAKE_ARGS[@]}"}"

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
