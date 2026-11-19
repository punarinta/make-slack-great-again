#!/usr/bin/env bash
# The FreeBSD and OpenBSD releases (amd64): msga cross-built on Linux with the
# host's clang + lld against the downloaded sysroot (build-bsd-cross.sh),
# stripped, and packed with its icon, a .desktop file and a README:
#   dist/msga-<os>-x86_64.tar.gz (+ .manifest, + .debug)
# The name carries no version, as the other platforms' assets: the website
# links https://msga.app/download/msga-<os>-x86_64.tar.gz for every release.
#
# Unlike the Linux release the binary is not static: it links the system's
# X11/Wayland, xkbcommon and D-Bus libraries, which the README names. Its
# FreeType and HarfBuzz are msga's own (MSGA_BUNDLED_FONT_LIBS). Self-update
# is off on the BSDs (MSGA_SELF_UPDATE): a new version is a new archive.
#
# Usage:
#   scripts/release-bsd.sh freebsd|openbsd
#
# Build tree (kept between runs, incremental): build-<os>-release/.
# Nothing runs the BSD binary here: start it on the BSD itself (README.txt).
# Requires: what build-bsd-cross.sh needs, plus objcopy (GNU or llvm-objcopy).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
OUT_DIR="${PROJECT_ROOT}/dist"

OS="${1:-}"
case "$OS" in
    freebsd) OS_NAME="FreeBSD" ;;
    openbsd) OS_NAME="OpenBSD" ;;
    *)
        echo "usage: $0 freebsd|openbsd" >&2
        exit 2
        ;;
esac
[[ $# -eq 1 ]] || { echo "usage: $0 freebsd|openbsd" >&2; exit 2; }

OBJCOPY=""
for c in llvm-objcopy objcopy; do
    command -v "$c" >/dev/null 2>&1 && { OBJCOPY="$c"; break; }
done
[[ -n "$OBJCOPY" ]] || { echo "error: objcopy (GNU binutils or llvm-objcopy) is required" >&2; exit 1; }

MSGA_VERSION="$(grep -m1 'MSGA_VERSION' "${PROJECT_ROOT}/version.cmake" | grep -oE '[0-9]+')"
NAME="msga-${OS}-x86_64"

# ── Build ─────────────────────────────────────────────────────────────────────
# -g + MSGA_LINK_STRIP=OFF keep the symbols for the .debug file (-g doesn't
# change the code); the map feeds the size report. Passed on every run: a
# build tree made without them is reconfigured.
"${SCRIPT_DIR}/build-bsd-cross.sh" "$OS" --release \
    -DCMAKE_C_FLAGS=-g -DCMAKE_CXX_FLAGS=-g -DMSGA_LINK_STRIP=OFF -DMSGA_SIZE_MAP=ON \
    -DMSGA_DEMO=OFF -DMSGA_SELF_UPDATE=OFF
BUILD_DIR="${PROJECT_ROOT}/build-${OS}-release"
BIN="${BUILD_DIR}/msga"

# ── Checks on the linked binary ───────────────────────────────────────────────
ABI="$(readelf -hW "$BIN" | awk -F: '/OS\/ABI/ { gsub(/^ +/, "", $2); print $2 }')"
NOTES="$(readelf -nW "$BIN" 2>/dev/null || true)"
case "$OS" in
    freebsd) [[ "$ABI" == *FreeBSD* ]] || { echo "error: ${BIN} is not a FreeBSD binary (${ABI})" >&2; exit 1; } ;;
    openbsd) grep -q OpenBSD <<<"$NOTES" || { echo "error: ${BIN} has no OpenBSD note" >&2; exit 1; } ;;
esac
readelf -SW "$BIN" | grep -q '\.debug_info' ||
    { echo "error: ${BIN} has no debug info (a tree configured without -g?)" >&2; exit 1; }
# The libraries it needs at run time: shown, and checked against what the
# README tells people to install.
NEEDED="$(readelf -dW "$BIN" | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p')"

if [[ -f "${BIN}.map" ]]; then
    python3 "${PROJECT_ROOT}/src/tools/size_report.py" "${BIN}.map" || true
fi

# ── dist/: the archive, its manifest and the symbols ─────────────────────────
# The .debug file is the only way to symbolise a user's crash.log (addr2line
# -e <name>.debug -fCi <address − executable base>): archive it per release,
# never upload it.
STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT
PKG="${STAGE}/${NAME}"
mkdir -p "$PKG/icons" "$OUT_DIR"
DEBUG="${OUT_DIR}/${NAME}.debug"
rm -f "$DEBUG" "${OUT_DIR}/${NAME}.tar.gz" "${OUT_DIR}/${NAME}.manifest"
"$OBJCOPY" --only-keep-debug "$BIN" "$DEBUG"
"$OBJCOPY" --strip-all --add-gnu-debuglink="$DEBUG" "$BIN" "$PKG/msga"
chmod 755 "$PKG/msga"
install -m 644 "${PROJECT_ROOT}/gfx/icon_256.png" "$PKG/icons/msga.png"
install -m 644 "${PROJECT_ROOT}/gfx/icon.svg" "$PKG/icons/msga.svg"
install -m 644 "${PROJECT_ROOT}/gfx/msga.desktop" "$PKG/msga.desktop"

if [[ "$OS" == freebsd ]]; then
    PKGS="libxkbcommon libxcb xcb-util-cursor wayland dbus noto-sans dejavu en-hunspell sndio"
    INSTALL="pkg install $PKGS"
    SETS="the base system (no X11 sets: the X libraries come from the packages)"
    PREFIX="/usr/local"
    AUDIO="sndio's aucat (package sndio; it plays through OSS), or PipeWire/PulseAudio tools when installed"
    BUILT_FOR="FreeBSD 14 (built against 14.4, so 14.4 and later 14.x)"
else
    PKGS="libxkbcommon wayland dbus noto-fonts"
    INSTALL="pkg_add $PKGS"
    SETS="the xbase, xfont and xserv sets (X11 and its libraries, as on any OpenBSD desktop)"
    PREFIX="/usr/local"
    AUDIO="aucat, in the base system"
    BUILT_FOR="OpenBSD 7.9 only: each OpenBSD release renumbers its libraries, so a new release needs a new build"
fi
cat >"$PKG/README.txt" <<EOF
msga ${MSGA_VERSION} for ${OS_NAME} (amd64)
===================================

A fast native Slack client. https://msga.app

For ${BUILT_FOR}.

Runs on ${OS_NAME} with ${SETS}, plus these packages (as root):

    ${INSTALL}

  libxkbcommon, libxcb, xcb-util-cursor, wayland, dbus: the libraries msga
      links (the window, the keyboard, notifications and the tray). On
      OpenBSD the X libraries are in xbase, and msga runs on X11 (the wayland
      package is only linked).
  noto-sans / noto-fonts: msga's interface font (DejaVu is the fallback and
      the monospace face).
  en-hunspell: spell checking (FreeBSD; any Hunspell dictionary in
      /usr/local/share/hunspell or ~/.local/share/hunspell works, on OpenBSD
      copy en_US.aff/.dic there by hand or point DICPATH at them).
  Sound: ${AUDIO}.

Running
-------

    tar xzf ${NAME}.tar.gz
    ./${NAME}/msga

or install it for everyone:

    install -m 755 ${NAME}/msga ${PREFIX}/bin/msga
    install -m 644 ${NAME}/msga.desktop ${PREFIX}/share/applications/msga.desktop
    install -m 644 ${NAME}/icons/msga.png ${PREFIX}/share/icons/hicolor/256x256/apps/msga.png
    install -m 644 ${NAME}/icons/msga.svg ${PREFIX}/share/icons/hicolor/scalable/apps/msga.svg

On its first start msga also adds itself to your desktop's application menu
(~/.local/share/applications/msga.desktop, with its icon) and
registers the msga:// links the Slack sign-in comes back through, so the
system-wide install above is optional.

It runs under any X11 desktop; on FreeBSD under a Wayland compositor too.

Updates
-------

msga does not update itself on ${OS_NAME}: download the next archive and
replace the binary. Settings and sign-ins stay in ~/.config/msga and
~/.local/share/msga.

Claude Code sessions need the claude CLI, which has no ${OS_NAME} build: point
msga at a Linux or macOS machine that runs it (Settings, Claude Code, "Runs
on": another computer over SSH).
EOF

chmod 644 "$PKG/README.txt"
chmod 755 "$PKG" "$PKG/icons"
tar -C "$STAGE" -czf "${OUT_DIR}/${NAME}.tar.gz" --owner=0 --group=0 --numeric-owner "$NAME"

# ── Manifest ──────────────────────────────────────────────────────────────────
# As the other releases': the version and the asset's sha256 (here the
# archive's; nothing on the BSDs downloads it by itself).
printf '{"version":%s,"updatedAt":"%s","sha256":"%s"}\n' "$MSGA_VERSION" \
    "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$(sha256sum "${OUT_DIR}/${NAME}.tar.gz" | cut -d' ' -f1)" \
    >"${OUT_DIR}/${NAME}.manifest"

echo ""
echo "Release: ${OUT_DIR}/${NAME}.tar.gz (version ${MSGA_VERSION})"
ls -l "${OUT_DIR}/${NAME}.tar.gz" "${OUT_DIR}/${NAME}.manifest" "$DEBUG"
echo "Binary: $(stat -c %s "$PKG/msga") bytes stripped"
echo "Needs at run time:"
printf '    %s\n' $NEEDED
tar -tzvf "${OUT_DIR}/${NAME}.tar.gz"
echo "Symbols: ${DEBUG} (NOT for upload — archive it per release)"
echo "Not started here: run it on ${OS_NAME} (README.txt) before publishing."
