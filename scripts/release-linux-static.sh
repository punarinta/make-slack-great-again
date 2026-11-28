#!/usr/bin/env bash
# The Linux release: a fully static msga (musl, static-PIE) that runs on any
# x86_64 distribution → dist/msga-linux-x86_64 (+ .manifest, + .debug).
#
# Built in Docker on Alpine (musl) with clang + lld, against static archives of
# everything plat links (xcb, xkbcommon, wayland, dbus); FreeType and HarfBuzz
# come from msga's own CMake (src/text/font_libs.cmake).
# The builder image is made once (~5 min) and reused; its tag hashes the
# Dockerfile below, so editing it rebuilds the image by itself.
#
# Usage:
#   scripts/release-linux-static.sh [--skip-tests] [--rebuild-image]
#
#   --skip-tests     Skip the test suite (it runs in the container, on musl).
#   --rebuild-image  Rebuild the builder image even if it exists.
#
# Env: NPROC=<jobs> (default: nproc)
# Requires: Docker. Optional: xvfb-run (X11 smoke launch), a Wayland session
# (Wayland smoke launch).
#
# Build trees (kept between runs, incremental): build-static/ (msga),
# build-static-tests/ (the test suite).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
OUT_DIR="${PROJECT_ROOT}/dist"
OUTPUT_NAME="msga-linux-x86_64"
NPROC="${NPROC:-$(nproc)}"
SKIP_TESTS=0
REBUILD_IMAGE=0

for arg in "$@"; do
    case "$arg" in
        --skip-tests) SKIP_TESTS=1 ;;
        --rebuild-image) REBUILD_IMAGE=1 ;;
        *) echo "error: unknown argument: $arg" >&2; exit 2 ;;
    esac
done

command -v docker >/dev/null 2>&1 || {
    echo "error: docker is required — https://docs.docker.com/get-docker/" >&2
    exit 1
}

# ── Builder image ─────────────────────────────────────────────────────────────
# Static archives of what plat links (FreeType and HarfBuzz are built by msga's
# own CMake, src/text/font_libs.cmake). Built here from source: Alpine has no
# .a for some (libXau, xcb-util, dbus) and ships others in a form lld can't use
# or bigger than they need to be:
#   wayland      Alpine's wayland-static is GCC LTO bitcode (stub .text), which
#                ld.lld can't read: every wl_* symbol ends up undefined.
#   libxcb,      one object per protocol (xinput, xkb, randr, render, …), so
#   xkbcommon    whole protocols link in for a few requests (~200 KB more).
# Everything -Os with -ffunction-sections/-fdata-sections, so --gc-sections
# drops what msga never calls. The .so files stay (the tools need some);
# MSGA_STATIC makes CMake pick the .a.
read -r -d '' DOCKERFILE <<'DOCKERFILE' || true
FROM alpine:3.23

RUN apk add --no-cache \
    build-base clang lld cmake ninja meson python3 pkgconf linux-headers curl xz bison \
    procps-ng git \
    libxcb-dev libxdmcp-dev xcb-proto \
    xcb-util-cursor-dev xcb-util-image-dev xcb-util-renderutil-dev \
    libxkbcommon-dev \
    wayland-dev wayland-protocols libffi-dev \
    expat-dev expat-static

ENV CC=clang CXX=clang++ CFLAGS="-Os -ffunction-sections -fdata-sections" \
    CXXFLAGS="-Os -ffunction-sections -fdata-sections"
WORKDIR /src-build

# libXau (libxcb.a's auth dependency) and xcb-util (under xcb-image/-cursor/
# -renderutil): Alpine has no .a for either. xcb-image.pc doesn't name its
# xcb-util dependency (a shared link resolves it through the .so).
RUN curl -fL https://xorg.freedesktop.org/archive/individual/lib/libXau-1.0.11.tar.gz | tar xz && \
    cd libXau-1.0.11 && ./configure --prefix=/usr --disable-shared --enable-static && \
    make -j"$(nproc)" install && cd .. && rm -rf libXau-1.0.11
# libxcb and libxkbcommon: the same versions as Alpine's -dev headers.
RUN curl -fL https://xcb.freedesktop.org/dist/libxcb-1.17.0.tar.xz | tar xJ && \
    cd libxcb-1.17.0 && ./configure --prefix=/usr --disable-shared --enable-static \
        --disable-devel-docs --without-doxygen && \
    make -j"$(nproc)" install && cd .. && rm -rf libxcb-1.17.0
RUN curl -fL https://github.com/xkbcommon/libxkbcommon/archive/refs/tags/xkbcommon-1.12.2.tar.gz | tar xz && \
    mv libxkbcommon-xkbcommon-1.12.2 libxkbcommon-1.12.2 && cd libxkbcommon-1.12.2 && \
    meson setup build --prefix=/usr --libdir=lib --default-library=static --buildtype=minsize \
        -Denable-x11=true -Denable-wayland=false -Denable-docs=false -Denable-tools=false \
        -Denable-xkbregistry=false -Denable-bash-completion=false && \
    ninja -C build install && cd .. && rm -rf libxkbcommon-1.12.2
RUN curl -fL https://xcb.freedesktop.org/dist/xcb-util-0.4.1.tar.gz | tar xz && \
    cd xcb-util-0.4.1 && ./configure --prefix=/usr --disable-shared --enable-static && \
    make -j"$(nproc)" install && cd .. && rm -rf xcb-util-0.4.1 && \
    sed -i 's/^Requires: xcb xcb-shm$/Requires: xcb xcb-shm\nRequires.private: xcb-util/' \
        /usr/lib/pkgconfig/xcb-image.pc && grep -q 'Requires.private: xcb-util' /usr/lib/pkgconfig/xcb-image.pc

# dbus: no dbus-static package.
RUN curl -fL https://dbus.freedesktop.org/releases/dbus/dbus-1.14.10.tar.xz | tar xJ && \
    cd dbus-1.14.10 && \
    ./configure --prefix=/usr --sysconfdir=/etc --localstatedir=/var \
        --disable-shared --enable-static --without-x --with-xml=expat \
        --disable-doxygen-docs --disable-xml-docs --disable-tests --disable-asserts \
        --disable-systemd --disable-selinux --disable-libaudit && \
    make -j"$(nproc)" install && cd .. && rm -rf dbus-1.14.10

RUN curl -fL https://gitlab.freedesktop.org/wayland/wayland/-/releases/1.24.0/downloads/wayland-1.24.0.tar.xz | tar xJ && \
    cd wayland-1.24.0 && \
    meson setup build --prefix=/usr --libdir=lib --default-library=static --buildtype=minsize \
        -Dtests=false -Ddocumentation=false -Ddtd_validation=false -Dscanner=true -Dlibraries=true && \
    ninja -C build install && cd .. && rm -rf wayland-1.24.0 && \
    ! readelf -SW /usr/lib/libwayland-client.a | grep -q gnu.lto

WORKDIR /
DOCKERFILE

IMAGE="msga-static-builder:$(printf '%s' "$DOCKERFILE" | sha256sum | cut -c1-12)"
if [[ $REBUILD_IMAGE -eq 1 ]]; then
    docker image rm -f "$IMAGE" >/dev/null 2>&1 || true
fi
if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
    echo "Building the builder image ${IMAGE} (first run only, a few minutes)..."
    printf '%s\n' "$DOCKERFILE" | docker build --tag "$IMAGE" -
fi

# ── Build (and test) in the container ─────────────────────────────────────────
# The source is mounted read-only; build trees live in the project (as the
# invoking user, so nothing root-owned is left behind). -g + MSGA_LINK_STRIP=OFF
# keep the symbols for the .debug file; -g doesn't change the code. The text
# and UI tests render with real fonts: the host's, mounted read-only.
CONFIGURE="-G Ninja -DCMAKE_BUILD_TYPE=MinSizeRel -DMSGA_STATIC=ON -DMSGA_DEMO=OFF"
CONFIGURE+=" -DCMAKE_C_FLAGS=-g -DCMAKE_CXX_FLAGS=-g -DMSGA_LINK_STRIP=OFF -DMSGA_SIZE_MAP=ON"
# A new image can have other libraries: drop the CMake caches that found them.
for dir in build-static build-static-tests; do
    mkdir -p "${PROJECT_ROOT}/${dir}"
    if [[ "$(cat "${PROJECT_ROOT}/${dir}/.builder-image" 2>/dev/null)" != "$IMAGE" ]]; then
        rm -f "${PROJECT_ROOT}/${dir}/CMakeCache.txt"
        echo "$IMAGE" >"${PROJECT_ROOT}/${dir}/.builder-image"
    fi
done
docker run --rm \
    --user "$(id -u):$(id -g)" \
    --env HOME=/tmp \
    --volume "${PROJECT_ROOT}:/src:ro" \
    --volume "${PROJECT_ROOT}/build-static:/build" \
    --volume "${PROJECT_ROOT}/build-static-tests:/build-tests" \
    --volume /usr/share/fonts:/usr/share/fonts:ro \
    "$IMAGE" sh -euc "
        if [ ${SKIP_TESTS} -eq 0 ]; then
            echo 'Running the test suite (musl)...'
            cmake -S /src -B /build-tests ${CONFIGURE} -DMSGA_BUILD_TESTS=ON >/dev/null
            cmake --build /build-tests --parallel ${NPROC}
            ctest --test-dir /build-tests --output-on-failure -j ${NPROC}
        fi
        echo 'Building msga (static, musl)...'
        cmake -S /src -B /build ${CONFIGURE} -DMSGA_BUILD_TESTS=OFF
        cmake --build /build --target msga --parallel ${NPROC}
        python3 /src/src/tools/size_report.py /build/msga.map
    "

BIN="${PROJECT_ROOT}/build-static/msga"

# ── Checks on the linked binary ───────────────────────────────────────────────
# Static: no interpreter, no shared libraries.
if readelf -lW "$BIN" | grep -q 'INTERP' || readelf -dW "$BIN" | grep -q 'NEEDED'; then
    echo "error: ${BIN} is not fully static:" >&2
    readelf -dW "$BIN" | grep NEEDED >&2 || true
    exit 1
fi
# musl adopts PT_GNU_STACK's size as the default thread stack; under 8 MiB the
# threads are back on its 128 KiB default (issue #70).
STACK_MEMSZ=$(( $(readelf -lW "$BIN" | awk '/GNU_STACK/ { print $6 }' | grep . || echo 0) ))
if [[ "$STACK_MEMSZ" -lt 8388608 ]]; then
    echo "error: PT_GNU_STACK size is ${STACK_MEMSZ} (< 8 MiB): -z stack-size was dropped from the link (issue #70)" >&2
    exit 1
fi

# ── dist/: stripped binary + its symbols ──────────────────────────────────────
# The .debug file is the only way to symbolise a user's crash.log (addr2line
# -e msga-linux-x86_64.debug -fCi <address − executable base>): archive it per
# release, never upload it.
mkdir -p "$OUT_DIR"
OUT="${OUT_DIR}/${OUTPUT_NAME}"
rm -f "$OUT" "${OUT}.debug"
objcopy --only-keep-debug "$BIN" "${OUT}.debug"
objcopy --strip-all --add-gnu-debuglink="${OUT}.debug" "$BIN" "$OUT"
chmod 755 "$OUT"

# ── Smoke launches ────────────────────────────────────────────────────────────
# The binary must reach its event loop on X11 (Xvfb) and, when this session
# has one, on Wayland. A throwaway HOME/XDG and no session bus: no real
# settings, no workspaces, nothing in the desktop's tray.
# Still running when timeout's TERM arrives (rc 124) = success.
smoke() {
    local name="$1"
    shift
    local home log rc=0
    home="$(mktemp -d)"
    log="$(mktemp)"
    mkdir -p "${home}/run"
    chmod 700 "${home}/run"
    env -u DBUS_SESSION_BUS_ADDRESS HOME="$home" XDG_RUNTIME_DIR="${home}/run" \
        XDG_CONFIG_HOME="${home}/.config" \
        XDG_DATA_HOME="${home}/.local/share" XDG_CACHE_HOME="${home}/.cache" "$@" >"$log" 2>&1 || rc=$?
    if [[ $rc -eq 124 ]]; then
        echo "Smoke launch (${name}): OK, still running after 6 s."
    else
        echo "error: smoke launch (${name}) exited with ${rc}:" >&2
        sed 's/^/    /' "$log" >&2
        rm -rf "$home" "$log"
        exit 1
    fi
    rm -rf "$home" "$log"
}
if command -v xvfb-run >/dev/null 2>&1; then
    # timeout must wrap msga itself: xvfb-run never forwards the signal.
    smoke "X11, Xvfb" env -u WAYLAND_DISPLAY \
        xvfb-run -a timeout --kill-after=3 6 "$OUT"
else
    echo "warning: no xvfb-run — X11 smoke launch skipped. Run ${OUT} and check it opens." >&2
fi
if [[ -n "${WAYLAND_DISPLAY:-}" ]]; then
    # A private XDG_RUNTIME_DIR, or the single-instance socket there hands
    # off to an msga already running in this session; the compositor's socket
    # is passed by absolute path instead.
    WL_SOCKET="$WAYLAND_DISPLAY"
    [[ "$WL_SOCKET" == /* ]] || WL_SOCKET="${XDG_RUNTIME_DIR}/${WL_SOCKET}"
    smoke "Wayland" env -u DISPLAY WAYLAND_DISPLAY="$WL_SOCKET" \
        timeout --kill-after=3 6 "$OUT"
else
    echo "note: no WAYLAND_DISPLAY — Wayland smoke launch skipped." >&2
fi

# ── Version manifest ──────────────────────────────────────────────────────────
# The in-app updater (app/update) downloads <asset> and <asset>.manifest and
# rejects a download whose sha256 doesn't match.
MSGA_VERSION="$(grep -m1 'MSGA_VERSION' "${PROJECT_ROOT}/version.cmake" | grep -oE '[0-9]+')"
printf '{"version":%s,"updatedAt":"%s","sha256":"%s"}\n' "$MSGA_VERSION" \
    "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$(sha256sum "$OUT" | cut -d' ' -f1)" \
    >"${OUT}.manifest"

echo ""
echo "Release: ${OUT} (version ${MSGA_VERSION})"
ls -l "$OUT" "${OUT}.manifest" "${OUT}.debug"
file "$OUT"
echo "Symbols: ${OUT}.debug (NOT for upload — archive it per release)"
