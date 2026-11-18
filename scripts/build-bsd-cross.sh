#!/usr/bin/env bash
# Cross-build msga and its test binaries for FreeBSD or OpenBSD (amd64) on
# Linux, with the host's clang + lld against a downloaded sysroot.
# Usage:
#   scripts/build-bsd-cross.sh freebsd|openbsd [--release] [cmake -D… args]
#     default:   Debug test build (MSGA_BUILD_TESTS=ON) → build-<os>/
#     --release: MinSizeRel msga only                    → build-<os>-release/
# The sysroot (base system + the packages the desktop build links) is fetched
# once by scripts/bsd-sysroot.py into build-bsd-sysroot/<os>/ (gitignored;
# delete it to fetch again). Nothing in it is ever run. Needs: clang, lld,
# cmake, ninja, pkg-config, wayland-scanner and python3 on the host.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
NPROC="$(nproc 2>/dev/null || echo 4)"

OS="${1:-}"
case "$OS" in
    freebsd | openbsd) shift ;;
    *)
        echo "usage: $0 freebsd|openbsd [--release] [cmake args]" >&2
        exit 2
        ;;
esac
RELEASE=0
if [[ "${1:-}" == "--release" ]]; then
    RELEASE=1
    shift
fi

SYSROOT_DIR="${PROJECT_ROOT}/build-bsd-sysroot/${OS}"
if [[ ! -f "${SYSROOT_DIR}/root/.complete" ]]; then
    python3 -I "${SCRIPT_DIR}/bsd-sysroot.py" "$OS" "$SYSROOT_DIR"
fi

if [[ "$RELEASE" == "1" ]]; then
    BUILD_DIR="${PROJECT_ROOT}/build-${OS}-release"
    OPTIONS=(-DCMAKE_BUILD_TYPE=MinSizeRel)
else
    BUILD_DIR="${PROJECT_ROOT}/build-${OS}"
    OPTIONS=(-DCMAKE_BUILD_TYPE=Debug -DMSGA_BUILD_TESTS=ON)
fi
if [[ ! -f "${BUILD_DIR}/build.ninja" ]]; then
    rm -rf "$BUILD_DIR"
    cmake -S "$PROJECT_ROOT" -B "$BUILD_DIR" -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE="${PROJECT_ROOT}/src/plat/cmake/bsd-clang.cmake" \
        -DMSGA_BSD="$OS" -DMSGA_BSD_SYSROOT="${SYSROOT_DIR}/root" \
        "${OPTIONS[@]}" "$@"
elif [[ $# -gt 0 ]]; then
    cmake "$BUILD_DIR" "$@"
fi

cmake --build "$BUILD_DIR" --parallel "$NPROC"
echo "Built: ${BUILD_DIR}/msga"
