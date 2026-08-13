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

# Point find_package(Qt6) at a Qt that isn't on the default CMake search path
# (explicit QT_PREFIX, Homebrew's keg-only Qt, an online-installer kit) — see
# scripts/qt-prefix.sh. Same variable as release.sh/run-tests.sh/coverage.sh.
source "${SCRIPT_DIR}/qt-prefix.sh"
msga_resolve_qt_prefix
msga_qt_cmake_args
msga_qt_report
CMAKE_EXTRA+=("${MSGA_QT_CMAKE_ARGS[@]+"${MSGA_QT_CMAKE_ARGS[@]}"}")

# Only reconfigure when not already a Ninja build, or when the Qt it was
# configured against changed (setting QT_PREFIX after a failed build must not
# be silently ignored). Wipe the directory if it exists but has no build.ninja
# (stale or wrong generator).
if [[ ! -f "${BUILD_DIR}/build.ninja" ]] || msga_qt_prefix_changed "$BUILD_DIR"; then
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
