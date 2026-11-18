#!/usr/bin/env bash
# clang-format discovery + the version floor, shared by configure-linux.sh,
# configure-mac.sh, format.sh and .githooks/pre-commit. Source it, don't run it.
#
# Why a floor: .clang-format is read identically by every release, but LLVM 22
# changed the OUTPUT for two options we enable (AlignConsecutive* across
# multi-line declarations, BlockIndent wrapping `if (` conditions). A file
# formatted with 21 fails --dry-run under 22 and vice versa, so every
# contributor has to be on the same side of that line. The tree is formatted
# with 22+; Ubuntu's unversioned `clang-format` package is still 21.
#
# Bash 3.2 compatible (macOS): no mapfile, no associative arrays.

MSGA_CLANG_FORMAT_MIN=22

# msga_clang_format_major <binary> -> prints the major version, or nothing.
msga_clang_format_major() {
    "$1" --version 2>/dev/null | sed -n 's/.*version \([0-9][0-9]*\)\.[0-9].*/\1/p' | head -1
}

# Sets CLANG_FORMAT to the first binary on PATH whose major is >= the floor
# (unversioned name first, then clang-format-NN newest first) and
# MSGA_CLANG_FORMAT_VERSION to its `--version` line. Returns 1 when none
# qualifies; CLANG_FORMAT then names the newest one found (or is empty) so
# callers can say which version is too old.
msga_resolve_clang_format() {
    CLANG_FORMAT=""
    MSGA_CLANG_FORMAT_VERSION=""
    local best="" best_major=0 cand major
    for cand in clang-format clang-format-30 clang-format-29 clang-format-28 \
                clang-format-27 clang-format-26 clang-format-25 clang-format-24 \
                clang-format-23 clang-format-22 clang-format-21 clang-format-20 \
                clang-format26 clang-format25 clang-format24 clang-format23 \
                clang-format22; do # FreeBSD's llvmNN packages: no dash
        command -v "$cand" &>/dev/null || continue
        major=$(msga_clang_format_major "$cand")
        [[ -n "$major" ]] || continue
        if (( major >= MSGA_CLANG_FORMAT_MIN )); then
            CLANG_FORMAT="$cand"
            MSGA_CLANG_FORMAT_VERSION=$("$cand" --version 2>/dev/null | head -1)
            return 0
        fi
        if (( major > best_major )); then best="$cand"; best_major=$major; fi
    done
    CLANG_FORMAT="$best"
    [[ -n "$best" ]] && MSGA_CLANG_FORMAT_VERSION=$("$best" --version 2>/dev/null | head -1)
    return 1
}

# One-line install hint per platform, for error messages.
msga_clang_format_hint() {
    echo "Need clang-format >= ${MSGA_CLANG_FORMAT_MIN}. Ubuntu/Debian: sudo apt install clang-format-${MSGA_CLANG_FORMAT_MIN} (or https://apt.llvm.org/);"
    echo "macOS: brew install clang-format; FreeBSD: pkg install llvm${MSGA_CLANG_FORMAT_MIN}; anywhere with Python: pipx install clang-format"
}
