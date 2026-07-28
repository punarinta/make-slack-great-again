#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
NPROC="$(nproc 2>/dev/null || sysctl -n hw.ncpu)"

# --asan: build with AddressSanitizer + LeakSanitizer into a separate dir so the
# instrumented objects never mix with a normal build.
ASAN=0
for arg in "$@"; do
    case "$arg" in
        --asan) ASAN=1 ;;
        *) echo "unknown argument: $arg" >&2; exit 2 ;;
    esac
done

if [[ "$ASAN" == "1" ]]; then
    BUILD_DIR="${PROJECT_ROOT}/build-asan"
    CMAKE_EXTRA=(-DMSGA_ASAN=ON)
else
    BUILD_DIR="${PROJECT_ROOT}/build"
    CMAKE_EXTRA=()
fi

# Homebrew keeps Qt keg-only (off the default CMake search path), so point
# find_package(Qt6) at it here instead of relying on the caller to export
# CMAKE_PREFIX_PATH. No-op where brew or the qt formula is absent (e.g. Linux).
if command -v brew >/dev/null 2>&1; then
    QT_PREFIX="$(brew --prefix qt 2>/dev/null || true)"
    [[ -n "${QT_PREFIX}" ]] && CMAKE_EXTRA+=(-DCMAKE_PREFIX_PATH="${QT_PREFIX}")
fi

# Only reconfigure when not already a Ninja build.
# Wipe the directory if it exists but has no build.ninja (stale or wrong generator).
if [[ ! -f "${BUILD_DIR}/build.ninja" ]]; then
    rm -rf "${BUILD_DIR}"
    # ${CMAKE_EXTRA[@]+…} guards the empty-array expansion so it doesn't trip
    # `set -u` on bash 3.2 (the /bin/bash that ships with macOS).
    cmake -S "$PROJECT_ROOT" -B "$BUILD_DIR" \
        -G Ninja \
        -DCMAKE_BUILD_TYPE=Debug \
        "${CMAKE_EXTRA[@]+"${CMAKE_EXTRA[@]}"}"
fi

cmake --build "$BUILD_DIR" --target msga --parallel "$NPROC"

# macOS 26+ enforces code signing even for local ad-hoc builds. This is done in
# a CMake POST_BUILD step (see the APPLE block in CMakeLists.txt) so it runs on
# every build — including a bare `cmake --build` — right after the Info.plist is
# installed, which is what gives the bundle a stable com.nisdos.msga identity.

if [[ "$ASAN" == "1" ]]; then
    cat <<EOF

ASan build ready: ${BUILD_DIR}/msga
Run with leak detection (logs to /tmp/msga-asan/asan.log.<pid>):

  mkdir -p /tmp/msga-asan
  ASAN_OPTIONS=detect_leaks=1:fast_unwind_on_malloc=0:log_path=/tmp/msga-asan/asan.log \\
  LSAN_OPTIONS=report_objects=1:suppressions=${SCRIPT_DIR}/lsan-suppressions.txt:print_suppressions=0 \\
    ${BUILD_DIR}/msga

(scripts/lsan-suppressions.txt hides known third-party startup leaks so the
report only flags leaks in our own code; or just use scripts/run-asan.sh.)
EOF
fi
