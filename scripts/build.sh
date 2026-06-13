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

# Only reconfigure when not already a Ninja build.
# Wipe the directory if it exists but has no build.ninja (stale or wrong generator).
if [[ ! -f "${BUILD_DIR}/build.ninja" ]]; then
    rm -rf "${BUILD_DIR}"
    cmake -S "$PROJECT_ROOT" -B "$BUILD_DIR" \
        -G Ninja \
        -DCMAKE_BUILD_TYPE=Debug \
        "${CMAKE_EXTRA[@]}"
fi

cmake --build "$BUILD_DIR" --target msga --parallel "$NPROC"

# macOS 26+ enforces code signing even for local ad-hoc builds.
if [[ "$(uname)" == "Darwin" ]]; then
    codesign --force --deep --sign - "${BUILD_DIR}/msga.app" 2>/dev/null
fi

if [[ "$ASAN" == "1" ]]; then
    cat <<EOF

ASan build ready: ${BUILD_DIR}/msga
Run with leak detection (logs to /tmp/msga-asan/asan.log.<pid>):

  mkdir -p /tmp/msga-asan
  ASAN_OPTIONS=detect_leaks=1:fast_unwind_on_malloc=0:log_path=/tmp/msga-asan/asan.log \\
  LSAN_OPTIONS=report_objects=1 \\
    ${BUILD_DIR}/msga
EOF
fi
