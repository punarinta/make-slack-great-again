#!/usr/bin/env bash
# Install what building msga needs on Debian/Ubuntu, then check it.
#
# Usage:
#   scripts/configure-linux.sh            # build + test dependencies
#   scripts/configure-linux.sh --release  # also the release tools: Docker
#                                         # (release-linux-static.sh,
#                                         # release-windows.sh), xvfb and wine
#                                         # (their smoke launches)
set -euo pipefail

GREEN='\033[0;32m'; YELLOW='\033[1;33m'; RED='\033[0;31m'; NC='\033[0m'
ok()   { printf "  ${GREEN}ok${NC}    %s\n" "$*"; }
miss() { printf "  ${YELLOW}miss${NC}  %s\n" "$*"; }
# Each argument is one line: "$*" would join a multi-line explanation into one.
die()  { printf "  ${RED}error${NC} %s\n" "$1" >&2; shift; (($#)) && printf "        %s\n" "$@" >&2; exit 1; }

CMAKE_MIN_MAJOR=3
CMAKE_MIN_MINOR=21

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
# clang-format discovery + its version floor, shared with format.sh and the hook.
source "${SCRIPT_DIR}/clang-format-env.sh"

RELEASE=0
for arg in "$@"; do
    case "$arg" in
        --release) RELEASE=1 ;;
        *) die "unknown argument: $arg" ;;
    esac
done

if ! command -v apt-get &>/dev/null; then
    die "This script requires apt (Debian/Ubuntu)." \
        "Elsewhere install the equivalents of: a C++20 compiler (clang + lld preferred), cmake," \
        "ninja, pkg-config, python3, and the development packages of freetype2, harfbuzz," \
        "xkbcommon(-x11), wayland-client/-cursor + wayland-protocols + wayland-scanner, dbus-1" \
        "and xcb with its xkb, shm, randr, cursor, xfixes, xinput and xtest extensions."
fi

APT_PACKAGES=(
    build-essential      # g++, make, binutils
    clang                # the release toolchain (-Oz is ~20% smaller than GCC's)
    lld                  # --gc-sections/--icf links and the size report's map
    cmake
    ninja-build
    pkgconf
    python3              # icon/translation generators, test servers (fake Slack/LLM)
    git
    libfreetype-dev      # text: dev builds use the system FreeType/HarfBuzz
    libharfbuzz-dev
    libxkbcommon-dev     # plat: keyboard (both backends)
    libxkbcommon-x11-dev
    libwayland-dev       # plat: Wayland backend (+ wayland-scanner)
    wayland-protocols
    libxcb1-dev          # plat: X11 backend, pure XCB
    libxcb-xkb-dev
    libxcb-shm0-dev
    libxcb-randr0-dev
    libxcb-cursor-dev
    libxcb-xfixes0-dev
    libxcb-xinput-dev
    libxcb-xtest0-dev    # test builds only: input injection
    libdbus-1-dev        # tray, notifications, portals
)
if [[ $RELEASE -eq 1 ]]; then
    APT_PACKAGES+=(
        xvfb             # xvfb-run: the release scripts' X11/Wine smoke launches
        wine             # release-windows.sh's smoke launch
    )
fi

to_install=()
for pkg in "${APT_PACKAGES[@]%%#*}"; do # strip inline comments
    pkg="${pkg// /}"
    [[ -z "$pkg" ]] && continue
    if dpkg-query -W -f='${Status}' "$pkg" 2>/dev/null | grep -q 'install ok installed'; then
        ok "$pkg"
    else
        miss "$pkg"
        to_install+=("$pkg")
    fi
done

if [[ ${#to_install[@]} -gt 0 ]]; then
    echo ""
    echo "Installing: ${to_install[*]}"
    sudo apt-get update -q
    sudo apt-get install -y "${to_install[@]}"
fi

# clang-format: the tree is formatted with >= ${MSGA_CLANG_FORMAT_MIN}, and older
# releases produce DIFFERENT output from the same .clang-format (see
# clang-format-env.sh), so the unversioned `clang-format` package — still 21 on
# Ubuntu 26.04 — is deliberately not in APT_PACKAGES. Take the newest versioned
# package the archive has; older LTS releases need https://apt.llvm.org/.
if msga_resolve_clang_format; then
    ok "clang-format (${MSGA_CLANG_FORMAT_VERSION}, >= ${MSGA_CLANG_FORMAT_MIN} required)"
else
    cf_pkg=""
    for v in 24 23 22; do
        if apt-cache show "clang-format-$v" &>/dev/null; then cf_pkg="clang-format-$v"; break; fi
    done
    if [[ -n "$cf_pkg" ]]; then
        if [[ -n "$CLANG_FORMAT" ]]; then
            miss "clang-format too old (${MSGA_CLANG_FORMAT_VERSION}) — installing $cf_pkg"
        else
            miss "clang-format — installing $cf_pkg"
        fi
        sudo apt-get install -y "$cf_pkg"
        if msga_resolve_clang_format; then
            ok "clang-format (${MSGA_CLANG_FORMAT_VERSION})"
        else
            die "$cf_pkg installed but no clang-format >= ${MSGA_CLANG_FORMAT_MIN} found on PATH."
        fi
    else
        miss "clang-format >= ${MSGA_CLANG_FORMAT_MIN} — not in this distro's archive"
        echo "        Add https://apt.llvm.org/ and install clang-format-${MSGA_CLANG_FORMAT_MIN}, or: pipx install clang-format"
        echo "        Do NOT format with an older release: its output differs and the pre-commit hook rejects it."
    fi
fi

# cmake version check (apt may provide an outdated cmake on older distros)
cmake_ver=$(cmake --version | awk 'NR==1{print $3}')
cmake_maj=$(echo "$cmake_ver" | cut -d. -f1)
cmake_min=$(echo "$cmake_ver" | cut -d. -f2)
if [[ "$cmake_maj" -gt "$CMAKE_MIN_MAJOR" ]] ||
    [[ "$cmake_maj" -eq "$CMAKE_MIN_MAJOR" && "$cmake_min" -ge "$CMAKE_MIN_MINOR" ]]; then
    ok "cmake $cmake_ver (>= ${CMAKE_MIN_MAJOR}.${CMAKE_MIN_MINOR} required)"
else
    echo ""
    die "cmake $cmake_ver is too old — need >= ${CMAKE_MIN_MAJOR}.${CMAKE_MIN_MINOR}." \
        "Install a newer version from https://cmake.org/download/ or the Kitware APT repository:" \
        "  https://apt.kitware.com/"
fi

# Docker: not installed from here — distributions package it as docker.io,
# Docker's own repository as docker-ce, and either is fine. The release scripts
# run it as the invoking user, so that user must be able to reach the daemon.
if [[ $RELEASE -eq 1 ]]; then
    if ! command -v docker &>/dev/null; then
        miss "docker — install docker.io (sudo apt-get install docker.io) or Docker Engine:"
        echo "        https://docs.docker.com/engine/install/"
    elif ! docker info &>/dev/null; then
        miss "docker is installed but this user can't reach the daemon. Add yourself to the"
        echo "        docker group (sudo usermod -aG docker \"\$USER\", then log in again), or start it."
    else
        ok "docker $(docker version --format '{{.Server.Version}}' 2>/dev/null)"
    fi
fi

if [[ -d "${PROJECT_ROOT}/.git" || -f "${PROJECT_ROOT}/.git" ]]; then
    git -C "$PROJECT_ROOT" config core.hooksPath .githooks
    ok "git hooks (.githooks/pre-commit)"
fi
if [[ ! -f "${PROJECT_ROOT}/credentials.cmake" ]]; then
    miss "credentials.cmake — builds work without it, but sign-in needs the Slack app keys"
    echo "        (copy credentials.cmake.example and fill it in)"
fi

echo ""
echo "Done. Build and test with:"
echo "  scripts/build.sh --test"
if [[ $RELEASE -eq 1 ]]; then
    echo "Releases:"
    echo "  scripts/release-linux-static.sh   scripts/release-windows.sh   scripts/release-mac-remote.sh"
fi
