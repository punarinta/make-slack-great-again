#!/usr/bin/env bash
# Shared Qt discovery for the build/test/release scripts — source, don't run:
#
#   source "${SCRIPT_DIR}/qt-prefix.sh"
#   msga_resolve_qt_prefix              # sets QT_PREFIX (may stay empty)
#   msga_qt_cmake_args                  # sets the MSGA_QT_CMAKE_ARGS array
#   msga_qt_prefix_changed "$BUILD_DIR" # true if that dir was configured
#                                       # against a different Qt
#
# QT_PREFIX is the one knob on every platform: an explicit value always wins,
# otherwise we look for a Qt that clears our 6.5 floor (Homebrew keeps Qt
# keg-only on macOS; on Linux the distro package is often too old and the real
# Qt sits in an online-installer kit under ~/Qt). An empty QT_PREFIX means "the
# system Qt is fine, let CMake find it".
#
# The 6.5 floor is QWebSocket::errorOccurred; an older Qt fails deep into the
# build with "not a member of QWebSocket", which names neither Qt nor a version.

if [[ "${BASH_SOURCE[0]}" == "$0" ]]; then
    echo "qt-prefix.sh is a helper for the build scripts — source it, don't run it." >&2
    exit 2
fi

MSGA_QT_MIN_MAJOR=6
MSGA_QT_MIN_MINOR=5
MSGA_QT_MIN="${MSGA_QT_MIN_MAJOR}.${MSGA_QT_MIN_MINOR}"

# Prints the first argument as the error, the rest indented under it — multi-line
# arguments (the install hint) get every line indented, not just the first.
_msga_qt_die() {
    local line
    printf 'error: %s\n' "$1" >&2
    shift
    while (($#)); do
        while IFS= read -r line; do printf '       %s\n' "$line" >&2; done <<< "$1"
        shift
    done
    exit 1
}

# Install hints, shared by every failure path below.
_msga_qt_install_hint() {
    printf '%s\n' \
        "Install Qt ${MSGA_QT_MIN}+ with the WebSockets module:" \
        "  Debian/Ubuntu: sudo apt install qt6-base-dev qt6-websockets-dev qt6-svg-dev" \
        "  macOS:         brew install qt" \
        "  Any distro:    https://www.qt.io/download-qt-installer" \
        "                 — tick 'Qt WebSockets', it is NOT selected by default" \
        "Then point the build at it (works for build.sh, release.sh, run-tests.sh," \
        "coverage.sh and a plain cmake invocation):" \
        "  QT_PREFIX=\$HOME/Qt/<version>/gcc_64 ./scripts/build.sh"
}

# Qt version of the kit at $1, empty when there is no usable Qt there.
_msga_qt_version_at() {
    local qmake
    for qmake in "$1/bin/qmake6" "$1/bin/qmake"; do
        if [[ -x "$qmake" ]]; then
            "$qmake" -query QT_VERSION 2>/dev/null
            return
        fi
    done
}

# True when version $1 clears the floor.
_msga_qt_at_least() {
    local maj="${1%%.*}" rest="${1#*.}" min
    min="${rest%%.*}"
    case "${maj}.${min}" in
        ''|*[!0-9.]*) return 1 ;;
    esac
    [[ "$maj" -gt "$MSGA_QT_MIN_MAJOR" ]] && return 0
    [[ "$maj" -eq "$MSGA_QT_MIN_MAJOR" && "$min" -ge "$MSGA_QT_MIN_MINOR" ]]
}

# True when version $1 is newer than $2 (both x.y.z).
_msga_qt_version_gt() {
    local a b i av bv
    for i in 1 2 3; do
        av="$(printf '%s' "$1" | cut -d. -f"$i")"
        bv="$(printf '%s' "$2" | cut -d. -f"$i")"
        a="${av:-0}"; b="${bv:-0}"
        [[ "$a" =~ ^[0-9]+$ ]] || a=0
        [[ "$b" =~ ^[0-9]+$ ]] || b=0
        [[ "$a" -gt "$b" ]] && return 0
        [[ "$a" -lt "$b" ]] && return 1
    done
    return 1
}

# True when the kit at $1 provides CMake config files for Qt6 module $2
# ("Qt6" itself for the base package). Handles both the plain layout used by
# installer kits and Homebrew, and Debian's multiarch lib/<triple>/cmake.
_msga_qt_module_present() {
    local prefix="$1" pkg="Qt6$2" f
    for f in "$prefix"/lib/cmake/"$pkg"/"$pkg"Config.cmake \
             "$prefix"/lib/*/cmake/"$pkg"/"$pkg"Config.cmake; do
        [[ -f "$f" ]] && return 0
    done
    return 1
}

# Reject an unusable explicit QT_PREFIX here rather than 100 compiler errors later.
_msga_qt_validate_prefix() {
    local prefix="$1" ver
    if [[ ! -d "$prefix" ]]; then
        _msga_qt_die "QT_PREFIX=$prefix does not exist." "$(_msga_qt_install_hint)"
    fi
    if ! _msga_qt_module_present "$prefix" ""; then
        _msga_qt_die "QT_PREFIX=$prefix has no Qt6Config.cmake (lib/cmake/Qt6/)." \
            "Point QT_PREFIX at a kit directory, e.g. \$HOME/Qt/6.9.0/gcc_64 — not at \$HOME/Qt." \
            "$(_msga_qt_install_hint)"
    fi
    ver="$(_msga_qt_version_at "$prefix")"
    if [[ -n "$ver" ]] && ! _msga_qt_at_least "$ver"; then
        _msga_qt_die "QT_PREFIX=$prefix is Qt $ver — msga needs ${MSGA_QT_MIN}+ (QWebSocket::errorOccurred)." \
            "$(_msga_qt_install_hint)"
    fi
    if ! _msga_qt_module_present "$prefix" WebSockets; then
        _msga_qt_die "QT_PREFIX=$prefix has no Qt WebSockets module." \
            "The online installer does not select it by default — re-run its Maintenance Tool," \
            "tick 'Qt WebSockets' under the Qt version you installed, then build again."
    fi
    MSGA_QT_VERSION="$ver"
}

# Newest online-installer kit that clears the floor and has WebSockets, if any.
_msga_qt_newest_kit() {
    local root dir ver best="" best_ver=""
    for root in "${HOME:-}/Qt" /opt/Qt /usr/local/Qt; do
        [[ -d "$root" ]] || continue
        for dir in "$root"/[0-9]*/*; do
            [[ -d "$dir" ]] || continue
            ver="$(_msga_qt_version_at "$dir")"
            [[ -n "$ver" ]] || continue
            _msga_qt_at_least "$ver" || continue
            _msga_qt_module_present "$dir" WebSockets || continue
            if [[ -z "$best_ver" ]] || _msga_qt_version_gt "$ver" "$best_ver"; then
                best="$dir"; best_ver="$ver"
            fi
        done
    done
    [[ -n "$best" ]] && printf '%s\n' "$best"
}

# Sets QT_PREFIX (possibly to ""), MSGA_QT_VERSION and MSGA_QT_SOURCE.
msga_resolve_qt_prefix() {
    MSGA_QT_VERSION=""
    MSGA_QT_SOURCE=""

    if [[ -n "${QT_PREFIX:-}" ]]; then
        QT_PREFIX="${QT_PREFIX%/}"
        _msga_qt_validate_prefix "$QT_PREFIX"
        MSGA_QT_SOURCE="QT_PREFIX"
        return
    fi

    # macOS: Homebrew's Qt is keg-only, so it is never on CMake's search path.
    # Only on macOS — on a Linux box with linuxbrew the distro Qt is the one the
    # user configured with configure-linux.sh, and it must keep winning.
    if [[ "${OSTYPE:-}" == darwin* ]] && command -v brew >/dev/null 2>&1; then
        local brew_qt
        brew_qt="$(brew --prefix qt 2>/dev/null || true)"
        if [[ -n "$brew_qt" && -d "$brew_qt" ]]; then
            QT_PREFIX="$brew_qt"
            MSGA_QT_VERSION="$(_msga_qt_version_at "$brew_qt")"
            MSGA_QT_SOURCE="Homebrew"
            return
        fi
    fi

    # A system Qt that already meets the floor needs no prefix at all.
    local sys_ver=""
    if command -v qmake6 >/dev/null 2>&1; then
        sys_ver="$(qmake6 -query QT_VERSION 2>/dev/null || true)"
    fi
    if [[ -n "$sys_ver" ]] && _msga_qt_at_least "$sys_ver"; then
        QT_PREFIX=""
        MSGA_QT_VERSION="$sys_ver"
        MSGA_QT_SOURCE="system"
        return
    fi

    # System Qt missing or too old — fall back to an installer kit if there is one.
    local kit
    kit="$(_msga_qt_newest_kit || true)"
    if [[ -n "$kit" ]]; then
        QT_PREFIX="$kit"
        MSGA_QT_VERSION="$(_msga_qt_version_at "$kit")"
        MSGA_QT_SOURCE="auto-detected"
        return
    fi

    if [[ -n "$sys_ver" ]]; then
        _msga_qt_die "Qt $sys_ver is too old — msga needs ${MSGA_QT_MIN}+ (QWebSocket::errorOccurred)." \
            "$(_msga_qt_install_hint)"
    fi

    # No qmake6 at all: Qt may still be installed (some distros don't ship it),
    # so let CMake look — its find_package failure names what is missing.
    QT_PREFIX=""
    MSGA_QT_SOURCE="unknown"
}

# Sets MSGA_QT_CMAKE_ARGS — expand with the ${x[@]+"${x[@]}"} guard so an empty
# array doesn't trip `set -u` on bash 3.2 (the /bin/bash macOS ships).
msga_qt_cmake_args() {
    if [[ -n "${QT_PREFIX:-}" ]]; then
        MSGA_QT_CMAKE_ARGS=(-DCMAKE_PREFIX_PATH="${QT_PREFIX}")
    else
        MSGA_QT_CMAKE_ARGS=()
    fi
}

msga_qt_report() {
    local where="${QT_PREFIX:-default search path}"
    printf 'Qt: %s (%s, %s)\n' "${MSGA_QT_VERSION:-version unknown}" "$where" "${MSGA_QT_SOURCE:-?}"
}

# True when $1 is an existing build dir configured against a different Qt.
# Without this a build dir configured before QT_PREFIX was set keeps silently
# using the old Qt: the scripts only reconfigure when the dir is missing.
msga_qt_prefix_changed() {
    local cache="$1/CMakeCache.txt" cached
    [[ -f "$cache" ]] || return 1
    cached="$(sed -n 's/^CMAKE_PREFIX_PATH:[^=]*=//p' "$cache" | head -1)"
    [[ "$cached" != "${QT_PREFIX:-}" ]]
}
