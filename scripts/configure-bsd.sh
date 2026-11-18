#!/bin/sh
# Install what building msga natively needs on FreeBSD (pkg) or OpenBSD
# (pkg_add), then check it. Plain sh: runs on a fresh install, before bash.
# Cross-building for the BSDs from Linux is scripts/build-bsd-cross.sh.
#
# Usage:
#   sh scripts/configure-bsd.sh
#
# Packages go in as root: directly when run as root, else through doas or
# sudo when one is set up for this user, else su (asks for root's password).
#
# Where the lists come from: the libraries are what plat's CMake fragments
# ask pkg-config for (src/plat/CMakeLists.txt, src/plat/src/{wayland/wayland,
# x11/x11,freedesktop/services}.cmake): xkbcommon(-x11), wayland-client and
# -cursor + wayland-protocols + wayland-scanner, dbus-1, and xcb with its xkb,
# shm, randr, cursor, xfixes, xinput and xtest (tests) extensions. FreeType
# and HarfBuzz are not on them: on the BSDs msga's CMake builds both itself
# (MSGA_BUNDLED_FONT_LIBS). The compiler and linker are the base system's
# clang and ld.lld on both.
set -eu

GREEN='\033[0;32m'; YELLOW='\033[1;33m'; RED='\033[0;31m'; NC='\033[0m'
ok()   { printf "  ${GREEN}ok${NC}    %s\n" "$*"; }
miss() { printf "  ${YELLOW}miss${NC}  %s\n" "$*"; }
# Each argument is one line: "$*" would join a multi-line explanation into one.
die() {
    printf "  ${RED}error${NC} %s\n" "$1" >&2
    shift
    [ $# -gt 0 ] && printf "        %s\n" "$@" >&2
    exit 1
}
hint() { printf "        %s\n" "$@"; }

CMAKE_MIN_MAJOR=3
CMAKE_MIN_MINOR=21
CLANG_FORMAT_MIN=22 # the floor in clang-format-env.sh (bash, so not sourced here)

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
PROJECT_ROOT=$(cd "$SCRIPT_DIR/.." && pwd)

[ $# -eq 0 ] || die "unknown argument: $1"

OS=$(uname -s)
case "$OS" in
    FreeBSD | OpenBSD) ;;
    Linux) die "This script is for FreeBSD and OpenBSD (Linux: configure-linux.sh)." ;;
    Darwin) die "This script is for FreeBSD and OpenBSD (macOS: configure-mac.sh)." ;;
    *) die "This script is for FreeBSD and OpenBSD, not $OS." ;;
esac

# How to run a command as root.
if [ "$(id -u)" -eq 0 ]; then
    AS_ROOT=""
elif command -v doas >/dev/null 2>&1 && [ -f /etc/doas.conf ]; then
    AS_ROOT="doas"
elif command -v sudo >/dev/null 2>&1; then
    AS_ROOT="sudo"
else
    AS_ROOT="su"
fi
as_root() {
    if [ "$AS_ROOT" = "su" ]; then
        echo "Neither doas nor sudo is set up for $(id -un): su asks for root's password."
        # Each argument single-quoted for su's shell (none has a quote in it).
        su root -c "$(printf "'%s' " "$@")" ||
            die "Installing packages needs root. Run this script as root, or install by hand: $*"
    else
        $AS_ROOT "$@"
    fi
}

# ── Packages ────────────────────────────────────────────────────────────────
# Lists are space-separated, with `name:why` comments kept out of them.
if [ "$OS" = "FreeBSD" ]; then
    # Base: clang/clang++, ld.lld (the default linker), make, the headers.
    # X11 is not in base on FreeBSD: libxcb ships every xcb extension the
    # X11 backend uses; xcb-util-cursor is xcb-cursor. libxkbcommon brings
    # xkbcommon-x11 and pulls in xkeyboard-config (the keymaps), wayland
    # pulls in libepoll-shim and libffi.
    PACKAGES="cmake ninja pkgconf git python3 bash
              wayland wayland-protocols libxkbcommon libxcb xcb-util-cursor dbus"
    # What a running msga (and the test suites) use when they are there:
    # fonts (anything under /usr/local/share/fonts: Noto Sans is msga's UI
    # face on Linux and the one the text and UI tests measure, DejaVu the
    # monospace one), the spell checker's en_US dictionary
    # (/usr/local/share/hunspell) and sndio's aucat for the notification
    # sound. A desktop install has most of them already.
    EXTRAS="noto-sans dejavu en-hunspell sndio"

    if [ ! -x /usr/bin/cc ] || [ ! -x /usr/bin/ld.lld ] || [ ! -f /usr/include/c++/v1/vector ]; then
        die "The base system's compiler is missing (/usr/bin/cc, /usr/bin/ld.lld or the C++ headers)." \
            "msga builds with base clang; reinstall the base set (it is part of every install)."
    fi
    ok "base clang ($(cc --version | head -1)), ld.lld"

    pkg_ready=0
    pkg -N >/dev/null 2>&1 && pkg_ready=1
    installed() { [ "$pkg_ready" -eq 1 ] && pkg info -e "$1" >/dev/null 2>&1; }
    install_pkgs() { as_root env ASSUME_ALWAYS_YES=yes pkg install -y "$@"; }
else
    # Base: clang/clang++ and ld.lld (OpenBSD's own lld, which finds the
    # libfoo.so.M.N libraries) and pkg-config. The comp set holds the headers
    # and the C++ library, the xbase set the X11 libraries: xcb with every
    # extension used, xcb-cursor among them.
    PACKAGES="cmake ninja git python%3 bash
              wayland wayland-protocols libxkbcommon dbus"
    # Noto Sans: msga's UI face and the one the text and UI tests measure
    # (the xfont set has only DejaVu). aucat is in base; a spell checking
    # dictionary, see the hint below.
    EXTRAS="noto-fonts"

    if [ ! -x /usr/bin/clang++ ] || [ ! -x /usr/bin/ld.lld ]; then
        die "The base system's clang++ or ld.lld is missing (/usr/bin)."
    fi
    if [ ! -f /usr/include/c++/v1/vector ]; then
        die "The comp set (headers, C++ library) is not installed." \
            "Add it with the installer's upgrade, or unpack comp$(uname -r | tr -d .).tgz from" \
            "the release's sets as root: tar xzphf comp*.tgz -C /"
    fi
    # X11 (Xenocara) comes from the release's sets, never from packages.
    if [ ! -f /usr/X11R6/lib/pkgconfig/xcb.pc ] || [ ! -f /usr/X11R6/lib/pkgconfig/xcb-cursor.pc ]; then
        die "The X11 sets are not installed (no xcb in /usr/X11R6): install xbase, xshare," \
            "xfont and xserv with the installer's upgrade, or unpack them from the release's" \
            "sets as root: tar xzphf xbase$(uname -r | tr -d .).tgz -C / (and the same for the others)"
    fi
    ok "base clang ($(cc --version | head -1)), ld.lld, comp and xbase sets"
    command -v pkg-config >/dev/null 2>&1 || die "pkg-config is missing from the base system."
    ok "pkg-config (base)"

    # pkg_add names: python%3 is the "python" stem's 3 branch.
    installed() {
        case "$1" in
            python%3) command -v python3 >/dev/null 2>&1 ;;
            *) pkg_info -e "$1-*" >/dev/null 2>&1 ;;
        esac
    }
    install_pkgs() { as_root pkg_add -I "$@"; }
fi

to_install=""
for pkg in $PACKAGES $EXTRAS; do
    if installed "$pkg"; then
        ok "$pkg"
    else
        miss "$pkg"
        to_install="$to_install $pkg"
    fi
done

if [ -n "$to_install" ]; then
    echo ""
    echo "Installing:$to_install"
    # shellcheck disable=SC2086 # word splitting is the point
    install_pkgs $to_install
    for pkg in $to_install; do
        installed "$pkg" || die "$pkg is still not installed."
    done
fi

for tool in cmake ninja pkg-config git python3 bash; do
    command -v "$tool" >/dev/null 2>&1 || die "$tool is not on PATH after installing (PATH=$PATH)."
done

# ── clang-format ────────────────────────────────────────────────────────────
# The tree is formatted with >= 22 and older releases produce DIFFERENT
# output from the same .clang-format (see clang-format-env.sh). Neither BSD
# ships one in base; the LLVM packages install it versioned:
# clang-formatNN on FreeBSD's llvmNN, /usr/local/llvmNN/bin on both.
cf_major() {
    "$1" --version 2>/dev/null | sed -n 's/.*version \([0-9][0-9]*\)\.[0-9].*/\1/p' | head -1
}
find_clang_format() {
    CF=""; CF_MAJOR=0
    for cand in clang-format clang-format30 clang-format29 clang-format28 clang-format27 \
                clang-format26 clang-format25 clang-format24 clang-format23 clang-format22 \
                clang-format-30 clang-format-29 clang-format-28 clang-format-27 clang-format-26 \
                clang-format-25 clang-format-24 clang-format-23 clang-format-22 \
                /usr/local/llvm*/bin/clang-format; do
        command -v "$cand" >/dev/null 2>&1 || [ -x "$cand" ] || continue
        m=$(cf_major "$cand")
        [ -n "$m" ] || continue
        if [ "$m" -gt "$CF_MAJOR" ]; then CF="$cand"; CF_MAJOR=$m; fi
    done
    [ "$CF_MAJOR" -ge "$CLANG_FORMAT_MIN" ]
}
cf_on_path_hint() {
    case "$CF" in
        /*) hint "It is not on PATH under a name format.sh looks for: add $(dirname "$CF") to PATH." ;;
    esac
}
if find_clang_format; then
    ok "clang-format $CF_MAJOR ($CF, >= $CLANG_FORMAT_MIN required)"
    cf_on_path_hint
else
    cf_pkg=""
    if [ "$OS" = "FreeBSD" ]; then
        for v in 24 23 22; do
            if pkg rquery '%n' "llvm$v" 2>/dev/null | grep -q .; then cf_pkg="llvm$v"; break; fi
        done
    fi
    if [ -n "$cf_pkg" ]; then
        if [ -n "$CF" ]; then
            miss "clang-format too old ($CF_MAJOR) — installing $cf_pkg (clang-format is part of it)"
        else
            miss "clang-format — installing $cf_pkg (clang-format is part of it)"
        fi
        install_pkgs "$cf_pkg"
        find_clang_format || die "$cf_pkg installed but no clang-format >= $CLANG_FORMAT_MIN found."
        ok "clang-format $CF_MAJOR ($CF)"
        cf_on_path_hint
    else
        miss "clang-format >= $CLANG_FORMAT_MIN — no LLVM package that new in this release's packages"
        if [ "$OS" = "OpenBSD" ]; then
            hint "pkg_info -Q llvm lists the LLVM packages; one >= $CLANG_FORMAT_MIN installs it under /usr/local/llvmNN/bin."
        fi
        hint "Anywhere with Python: pip install --user clang-format (puts it in ~/.local/bin)."
        hint "Only needed to format code. Do NOT format with an older release: its output differs and the pre-commit hook rejects it."
    fi
fi

# ── cmake version ───────────────────────────────────────────────────────────
cmake_ver=$(cmake --version | awk 'NR==1{print $3}')
cmake_maj=$(echo "$cmake_ver" | cut -d. -f1)
cmake_min=$(echo "$cmake_ver" | cut -d. -f2)
if [ "$cmake_maj" -gt "$CMAKE_MIN_MAJOR" ] ||
    { [ "$cmake_maj" -eq "$CMAKE_MIN_MAJOR" ] && [ "$cmake_min" -ge "$CMAKE_MIN_MINOR" ]; }; then
    ok "cmake $cmake_ver (>= ${CMAKE_MIN_MAJOR}.${CMAKE_MIN_MINOR} required)"
else
    echo ""
    die "cmake $cmake_ver is too old — need >= ${CMAKE_MIN_MAJOR}.${CMAKE_MIN_MINOR}." \
        "Update the package repository (pkg upgrade / pkg_add -u) or build cmake from source."
fi

# ── Libraries, as the build will look for them ─────────────────────────────
if pkg-config --exists xkbcommon xkbcommon-x11 wayland-client wayland-cursor wayland-scanner \
    wayland-protocols dbus-1 xcb xcb-xkb xcb-shm xcb-randr xcb-cursor xcb-xfixes xcb-xinput xcb-xtest; then
    ok "pkg-config finds xkbcommon, wayland, wayland-protocols, dbus-1 and xcb (+ extensions)"
else
    die "pkg-config can't find all of the libraries the build links; it says:" \
        "$(pkg-config --print-errors --exists xkbcommon xkbcommon-x11 wayland-client wayland-cursor \
            wayland-scanner wayland-protocols dbus-1 xcb xcb-xkb xcb-shm xcb-randr xcb-cursor \
            xcb-xfixes xcb-xinput xcb-xtest 2>&1 | head -3)"
fi

# ── Optional, at run time ───────────────────────────────────────────────────
if [ "$OS" = "OpenBSD" ]; then
    if [ -d /usr/X11R6/lib/X11/fonts/TTF ]; then
        ok "fonts (/usr/X11R6/lib/X11/fonts/TTF, the xfont set)"
    else
        miss "fonts — install the xfont set (DejaVu) or any TTF font package"
    fi
    if command -v aucat >/dev/null 2>&1; then ok "aucat (notification sound)"; fi
    miss "spell checking — copy en_US.aff/.dic (any Hunspell dictionary) into"
    hint "~/.local/share/hunspell, or point DICPATH at a folder that has them"
fi

# ── Repository ──────────────────────────────────────────────────────────────
# The hooks setting is written into .git/config: not as root into someone
# else's checkout, which would leave the file owned by root.
if [ -d "$PROJECT_ROOT/.git" ] || [ -f "$PROJECT_ROOT/.git" ]; then
    if [ "$(stat -f %u "$PROJECT_ROOT")" = "$(id -u)" ]; then
        git -C "$PROJECT_ROOT" config core.hooksPath .githooks
        ok "git hooks (.githooks/pre-commit)"
    else
        miss "git hooks — run as the checkout's owner: git config core.hooksPath .githooks"
    fi
fi
if [ ! -f "$PROJECT_ROOT/credentials.cmake" ]; then
    miss "credentials.cmake — builds work without it, but sign-in needs the Slack app keys"
    hint "(copy credentials.cmake.example and fill it in)"
fi

echo ""
echo "Done. Build and test with:"
echo "  scripts/build.sh --test"
