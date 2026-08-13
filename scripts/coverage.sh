#!/usr/bin/env bash
# Build the test suite with coverage instrumentation, run it, and produce a
# report. Mirrors run-tests.sh but uses a separate build dir and MSGA_COVERAGE.
#
# Requires gcovr (apt install gcovr / pipx install gcovr). The report covers
# only src/ files compiled into the test binaries — untested files won't appear.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# Canonicalize (no trailing "scripts/..") — ROOT is passed to gcovr --filter,
# which matches it literally against canonical source paths; a "scripts/.."
# segment would match nothing and silently report 0% coverage.
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$ROOT/build-cov"
NPROC="$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)"

if ! command -v gcovr >/dev/null 2>&1; then
    echo "error: gcovr not found. Install it with 'apt install gcovr' or 'pipx install gcovr'." >&2
    exit 1
fi

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
        -DMSGA_COVERAGE=ON \
        "${MSGA_QT_CMAKE_ARGS[@]+"${MSGA_QT_CMAKE_ARGS[@]}"}"
fi

cmake --build "$BUILD_DIR" --parallel "$NPROC"

( cd "$BUILD_DIR" && ctest --output-on-failure )

# Run gcovr from inside the build directory so the .gcno/.gcda compilation
# paths resolve. --filter restricts the report to our own sources (Qt headers,
# Catch2 and the FetchContent deps are excluded).
( cd "$BUILD_DIR" && gcovr --root "$ROOT" --filter "$ROOT/src/" \
    --exclude-unreachable-branches \
    --html-details "$BUILD_DIR/coverage.html" \
    --print-summary )

echo
echo "HTML report: ${BUILD_DIR}/coverage.html"
