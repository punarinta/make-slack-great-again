#!/usr/bin/env bash
# Build the test suite with coverage instrumentation (MSGA_COVERAGE), run it,
# and print a line/branch summary plus an HTML report. Linux and macOS; its own
# build dir, so it never shares objects with build/ or build-debug/.
# Usage:
#   scripts/coverage.sh                  # → build-cov/coverage.html
#   MSGA_COV_BUILD_DIR=<dir> scripts/coverage.sh
#
# Requires gcovr (apt install gcovr / pipx install gcovr). The report covers
# our own sources under src/ (plat included) that some test binary compiles in;
# a file no test links in does not appear. Vendored code is not instrumented
# (see MSGA_COVERAGE in CMakeLists.txt) and is excluded here as well.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# Canonical (no "scripts/.."): gcovr matches --root/--filter literally against
# canonical source paths, and a "scripts/.." would silently report 0%.
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${MSGA_COV_BUILD_DIR:-${PROJECT_ROOT}/build-cov}"
NPROC="$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)"

if ! command -v gcovr >/dev/null 2>&1; then
    echo "error: gcovr not found. Install it with 'apt install gcovr' or 'pipx install gcovr'." >&2
    exit 1
fi

# Only configure when not already a coverage Ninja build; wipe anything else
# (a stale dir, another generator, or another project's build-cov).
if [[ ! -f "${BUILD_DIR}/build.ninja" ]] ||
    ! grep -q '^MSGA_COVERAGE:BOOL=ON$' "${BUILD_DIR}/CMakeCache.txt" 2>/dev/null ||
    ! grep -q "^CMAKE_HOME_DIRECTORY:INTERNAL=${PROJECT_ROOT}\$" "${BUILD_DIR}/CMakeCache.txt"; then
    rm -rf "${BUILD_DIR}"
    cmake -S "$PROJECT_ROOT" -B "$BUILD_DIR" -G Ninja \
        -DCMAKE_BUILD_TYPE=Debug \
        -DMSGA_BUILD_TESTS=ON \
        -DMSGA_COVERAGE=ON
fi

cmake --build "$BUILD_DIR" --parallel "$NPROC"

# Counters from an earlier run would add up with this one's.
find "$BUILD_DIR" -name '*.gcda' -delete

# A failing test still leaves its counts: report anyway, exit with its status.
# Not the timing budgets (*_perf): -O0 plus counters is several times slower.
STATUS=0
ctest --test-dir "$BUILD_DIR" --output-on-failure -j "$NPROC" -E '_perf$' || STATUS=$?

# Clang writes LLVM's gcov flavour, which only its own llvm-cov reads.
GCOV=(gcov)
if "$(sed -n 's/^CMAKE_CXX_COMPILER:FILEPATH=//p' "${BUILD_DIR}/CMakeCache.txt")" --version 2>/dev/null | grep -q clang; then
    LLVM_COV="$(command -v llvm-cov || ls /usr/bin/llvm-cov-* 2>/dev/null | sort -V | tail -1 || true)"
    if [[ -z "$LLVM_COV" ]] && command -v xcrun >/dev/null 2>&1; then
        LLVM_COV="$(xcrun -f llvm-cov)"
    fi
    if [[ -z "$LLVM_COV" ]]; then
        echo "error: a Clang build needs llvm-cov for its .gcda files" >&2
        exit 1
    fi
    GCOV=("$LLVM_COV" gcov)
fi

# From inside the build dir, so the .gcno/.gcda compilation paths resolve.
# --filter keeps our own sources (system headers, the FetchContent deps and
# generated code all live outside src/); the excludes drop the vendored code in
# src/third_party and the tests themselves. --gcov-object-directory pins gcov's
# working directory to the build dir: without it, a gcov run that fails from
# --root makes gcovr retry from every parent of the .gcda up to /, leaving
# name##hash.gcov files in ~/src, ~ and wherever else gcov got to write.
( cd "$BUILD_DIR" && gcovr --root "$PROJECT_ROOT" \
    --gcov-executable "${GCOV[*]}" \
    --gcov-object-directory "$BUILD_DIR" \
    --filter "${PROJECT_ROOT}/src/" \
    --exclude "${PROJECT_ROOT}/src/third_party/" \
    --exclude "${PROJECT_ROOT}/tests/" \
    --exclude-unreachable-branches \
    -j "$NPROC" \
    --html-details "${BUILD_DIR}/coverage.html" \
    --print-summary )

echo
echo "HTML report: ${BUILD_DIR}/coverage.html"
exit "$STATUS"
