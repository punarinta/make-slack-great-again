#!/usr/bin/env bash
# The Windows release, cross-compiled on Linux: one self-contained msga.exe →
# dist/msga-windows-x86_64.exe (+ .manifest, + .debug).
#
# Built in Docker (Ubuntu 26.04, mingw-w64 13 GCC, posix threads) and linked fully
# static (MSGA_STATIC): libstdc++, libgcc and winpthread inside the .exe,
# FreeType and HarfBuzz built by msga's own CMake (src/text/font_libs.cmake),
# so it needs nothing but Windows' own DLLs. The builder image is small and
# made once; its tag hashes the Dockerfile below. On a Windows machine,
# scripts\release.ps1 builds the same thing natively with MSYS2.
#
# Usage:
#   scripts/release-windows.sh [--rebuild-image]
#
# Env: NPROC=<jobs> (default: nproc)
#      MSGA_SKIP_WINE_SMOKE=1  skip the Wine launch (only for a confirmed Wine
#                              quirk — then try the .exe on Windows)
# Requires: Docker. Optional: wine + xvfb-run (the startup smoke launch).
#
# Build tree (kept between runs, incremental): build-windows/.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
OUT_DIR="${PROJECT_ROOT}/dist"
OUTPUT_NAME="msga-windows-x86_64.exe"
BUILD_DIR="${PROJECT_ROOT}/build-windows"
NPROC="${NPROC:-$(nproc)}"
REBUILD_IMAGE=0

for arg in "$@"; do
    case "$arg" in
        --rebuild-image) REBUILD_IMAGE=1 ;;
        *) echo "error: unknown argument: $arg" >&2; exit 2 ;;
    esac
done

command -v docker >/dev/null 2>&1 || {
    echo "error: docker is required — https://docs.docker.com/get-docker/" >&2
    exit 1
}

# ── Builder image ─────────────────────────────────────────────────────────────
# mingw-w64 13 or newer: older headers lack the WinRT toast API
# (windows.ui.notifications.h), and plat then builds without notifications.
read -r -d '' DOCKERFILE <<'DOCKERFILE' || true
FROM ubuntu:26.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        g++-mingw-w64-x86-64-posix gcc-mingw-w64-x86-64-posix binutils-mingw-w64-x86-64 \
        cmake ninja-build python3 ca-certificates && \
    rm -rf /var/lib/apt/lists/*
DOCKERFILE

IMAGE="msga-windows-builder:$(printf '%s' "$DOCKERFILE" | sha256sum | cut -c1-12)"
if [[ $REBUILD_IMAGE -eq 1 ]]; then
    docker image rm -f "$IMAGE" >/dev/null 2>&1 || true
fi
if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
    echo "Building the builder image ${IMAGE} (first run only)..."
    printf '%s\n' "$DOCKERFILE" | docker build --tag "$IMAGE" -
fi

# ── Build ─────────────────────────────────────────────────────────────────────
# mingw links with ld.bfd (no lld ICF/map, see CMakeLists.txt), unstripped:
# -g for the .debug file, which doesn't change the code.
mkdir -p "$BUILD_DIR"
if [[ "$(cat "${BUILD_DIR}/.builder-image" 2>/dev/null)" != "$IMAGE" ]]; then
    rm -f "${BUILD_DIR}/CMakeCache.txt"
    echo "$IMAGE" >"${BUILD_DIR}/.builder-image"
fi
docker run --rm \
    --user "$(id -u):$(id -g)" \
    --env HOME=/tmp \
    --volume "${PROJECT_ROOT}:/src:ro" \
    --volume "${BUILD_DIR}:/build" \
    "$IMAGE" sh -euc "
        cmake -S /src -B /build -G Ninja \
            -DCMAKE_TOOLCHAIN_FILE=/src/src/plat/cmake/mingw-w64.cmake \
            -DCMAKE_BUILD_TYPE=MinSizeRel -DMSGA_STATIC=ON -DMSGA_DEMO=OFF -DMSGA_BUILD_TESTS=OFF \
            -DCMAKE_C_FLAGS=-g -DCMAKE_CXX_FLAGS=-g
        cmake --build /build --target msga --parallel ${NPROC}
    "

BIN="${BUILD_DIR}/msga.exe"

# Toast notifications compiled in (see the image's mingw-w64 note).
if ! grep -q 'ToastGeneric' "$BIN"; then
    echo "error: ${BIN} was built without toast notifications (mingw-w64 headers too old?)" >&2
    exit 1
fi
# Only Windows' own DLLs: a mingw runtime DLL here means the static link broke.
IMPORTS="$(objdump -p "$BIN" | awk '/DLL Name:/ { print tolower($3) }' | sort -u)"
if grep -qE '^(libstdc\+\+|libgcc|libwinpthread|libfreetype|libharfbuzz)' <<<"$IMPORTS"; then
    echo "error: ${BIN} imports mingw DLLs:" >&2
    echo "$IMPORTS" >&2
    exit 1
fi

# ── dist/: stripped .exe + its symbols ────────────────────────────────────────
# The .debug file resolves a user's crash.log (x86_64-w64-mingw32-addr2line
# -e msga-windows-x86_64.debug -fCi <address>): archive it per release, never
# upload it.
mkdir -p "$OUT_DIR"
OUT="${OUT_DIR}/${OUTPUT_NAME}"
DEBUG="${OUT_DIR}/msga-windows-x86_64.debug"
rm -f "$OUT" "$DEBUG"
docker run --rm --user "$(id -u):$(id -g)" \
    --volume "${BUILD_DIR}:/build:ro" --volume "${OUT_DIR}:/out" "$IMAGE" sh -euc "
        x86_64-w64-mingw32-objcopy --only-keep-debug /build/msga.exe /out/msga-windows-x86_64.debug
        x86_64-w64-mingw32-objcopy --strip-all \
            --add-gnu-debuglink=/out/msga-windows-x86_64.debug /build/msga.exe /out/${OUTPUT_NAME}
    "

# ── Smoke launch under Wine ───────────────────────────────────────────────────
# The only automated check that the .exe starts: under Xvfb, in a throwaway
# HOME + WINEPREFIX (registry, AppData and the msga:// registration land there,
# never in ~/.wine). A fresh prefix's C:\windows\Fonts is empty (Wine serves
# fonts virtually), and msga reads font files itself: a few real ones go in.
# WINEDLLOVERRIDES: no Mono/Gecko install dialogs, no host menu entries, and no
# winedbg dialog (a crash must exit, not sit there faking a healthy rc 124).
# Still running when timeout's TERM arrives (rc 124) = success; any other exit
# fails the release (a fresh prefix has no running instance to hand off to).
if [[ "${MSGA_SKIP_WINE_SMOKE:-0}" == "1" ]]; then
    echo "note: MSGA_SKIP_WINE_SMOKE=1 — Wine smoke launch skipped." >&2
elif command -v wine >/dev/null 2>&1 && command -v xvfb-run >/dev/null 2>&1; then
    echo "Smoke launch under Wine (Xvfb)..."
    SMOKE_HOME="$(mktemp -d)"
    SMOKE_LOG="$(mktemp)"
    SMOKE_RC=0
    FONTS="$(find /usr/share/fonts -name 'DejaVuSans.ttf' -o -name 'NotoSans-Regular.ttf' \
        -o -name 'LiberationSans-Regular.ttf' 2>/dev/null | head -3 | tr '\n' ':')"
    env -u DISPLAY -u WAYLAND_DISPLAY -u DBUS_SESSION_BUS_ADDRESS \
        HOME="$SMOKE_HOME" WINEPREFIX="${SMOKE_HOME}/wineprefix" WINEDEBUG=-all \
        WINEDLLOVERRIDES="mscoree=;mshtml=;winemenubuilder.exe=;winedbg.exe=" \
        SMOKE_BIN="$OUT" SMOKE_FONTS="$FONTS" \
        xvfb-run -a bash -c '
            timeout --kill-after=10 120 wineboot --init >/dev/null 2>&1 || true
            fonts="$WINEPREFIX/drive_c/windows/Fonts"
            mkdir -p "$fonts"
            IFS=: read -ra list <<<"$SMOKE_FONTS"
            for f in "${list[@]}"; do [ -n "$f" ] && cp "$f" "$fonts/"; done
            rc=0
            timeout --kill-after=5 --signal=TERM 20 wine "$SMOKE_BIN" || rc=$?
            wineserver -k >/dev/null 2>&1 || true
            wineserver -w >/dev/null 2>&1 || true
            exit $rc
        ' >"$SMOKE_LOG" 2>&1 || SMOKE_RC=$?
    # Wine's services can outlive wineserver -w by a beat and recreate files
    # mid-delete: retry once, and never fail a release over a temp dir.
    cleanup() {
        rm -rf "$SMOKE_HOME" "$SMOKE_LOG" 2>/dev/null ||
            { sleep 2; rm -rf "$SMOKE_HOME" "$SMOKE_LOG" 2>/dev/null || true; }
    }
    if [[ $SMOKE_RC -eq 124 ]]; then
        echo "Smoke launch (Wine): OK, still running after 20 s."
        cleanup
    else
        echo "error: ${OUTPUT_NAME} exited under Wine with ${SMOKE_RC}:" >&2
        sed 's/^/    /' "$SMOKE_LOG" >&2
        echo "  A Wine quirk rather than a broken .exe? MSGA_SKIP_WINE_SMOKE=1, then test on Windows." >&2
        cleanup
        exit 1
    fi
else
    echo "warning: no wine/xvfb-run — smoke launch skipped. Start ${OUT} on Windows and check it opens." >&2
fi

# ── Version manifest ──────────────────────────────────────────────────────────
# The in-app updater (app/update) downloads <asset> and msga-windows-x86_64
# .manifest and rejects a download whose sha256 doesn't match.
MSGA_VERSION="$(grep -m1 'MSGA_VERSION' "${PROJECT_ROOT}/version.cmake" | grep -oE '[0-9]+')"
printf '{"version":%s,"updatedAt":"%s","sha256":"%s"}\n' "$MSGA_VERSION" \
    "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$(sha256sum "$OUT" | cut -d' ' -f1)" \
    >"${OUT_DIR}/msga-windows-x86_64.manifest"

echo ""
echo "Release: ${OUT} (version ${MSGA_VERSION})"
ls -l "$OUT" "${OUT_DIR}/msga-windows-x86_64.manifest" "$DEBUG"
echo "Imports: $(tr '\n' ' ' <<<"$IMPORTS")"
echo "Symbols: ${DEBUG} (NOT for upload — archive it per release)"
