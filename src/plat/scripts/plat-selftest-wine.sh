#!/usr/bin/env bash
# Run plat's Windows test executables under Wine, on a private Xvfb display,
# with a throwaway Wine prefix — never the developer's display or ~/.wine.
#
#   src/plat/scripts/plat-selftest-wine.sh build-plat-win/plat_selftest.exe [more.exe ...]
#
# Exit status: that of the first failing exe, else 0 (so a selftest FAIL
# fails the script). Output is the exes' own PASS/FAIL/SKIP lines.
#
# Environment:
#   PLAT_WINEPREFIX  reuse this prefix (created on first use, kept afterwards)
#                    to skip the slow first-time `wineboot`; default: a fresh
#                    temporary prefix, deleted at the end.
#   PLAT_WINE_TIMEOUT  seconds per exe (default 120).
#   PLAT_SELFTEST_ALLOW_REGISTRATION  defaults to 1 here: the prefix and
#                    HOME are throwaway, so the tests may write HKCU (URL
#                    schemes, accessibility/accent values) — set it to empty
#                    to skip those checks.
#
# Wine limitations that show up as SKIPs or differ from real Windows: no DWM
# (no composition, shadow, Snap Layouts; DwmFlush is a no-op), no window
# manager on bare Xvfb (so no Aero-style move/resize loop), IME via XIM only.
set -euo pipefail

if [[ $# -lt 1 ]]; then
    echo "usage: $0 <exe> [exe ...]" >&2
    exit 2
fi
for exe in "$@"; do
    [[ -f "$exe" ]] || { echo "no such file: $exe" >&2; exit 2; }
done
command -v wine >/dev/null || { echo "wine not installed" >&2; exit 2; }
command -v Xvfb >/dev/null || { echo "Xvfb not installed" >&2; exit 2; }

# A display nobody uses: no socket and no lock file (≥ 80 keeps clear of
# real sessions and of xvfb-run's default 99 range).
display=""
for n in $(seq 80 160); do
    if [[ ! -e /tmp/.X11-unix/X$n && ! -e /tmp/.X$n-lock ]]; then
        display=$n
        break
    fi
done
[[ -n "$display" ]] || { echo "no free X display number" >&2; exit 2; }

work=$(mktemp -d "${TMPDIR:-/tmp}/plat-wine.XXXXXX")
xvfb_pid=""
keep_prefix=0
if [[ -n "${PLAT_WINEPREFIX:-}" ]]; then
    export WINEPREFIX="$PLAT_WINEPREFIX"
    keep_prefix=1
else
    export WINEPREFIX="$work/prefix"
fi

cleanup() {
    wineserver -k >/dev/null 2>&1 || true
    wineserver -w >/dev/null 2>&1 || true
    [[ -n "$xvfb_pid" ]] && kill "$xvfb_pid" 2>/dev/null && wait "$xvfb_pid" 2>/dev/null
    # Wine's services can outlive wineserver -w by a beat and recreate files
    # mid-delete; one retry is enough.
    rm -rf "$work" 2>/dev/null || { sleep 1; rm -rf "$work" 2>/dev/null || true; }
}
trap cleanup EXIT

# Isolation: Wine links the prefix's user folders to $HOME, and X/Wayland
# variables would let it reach the real desktop.
export HOME="$work/home"
mkdir -p "$HOME"
unset WAYLAND_DISPLAY XDG_SESSION_TYPE
export DISPLAY=":$display"
export WINEDEBUG=-all
export PLAT_SELFTEST_ALLOW_REGISTRATION="${PLAT_SELFTEST_ALLOW_REGISTRATION-1}"
[[ -n "$PLAT_SELFTEST_ALLOW_REGISTRATION" ]] || unset PLAT_SELFTEST_ALLOW_REGISTRATION
# mscoree/mshtml: no Mono/Gecko install dialogs (they block wineboot under
# Xvfb); winemenubuilder: no .desktop files leaking onto the host;
# winedbg: a crash exits non-zero instead of hanging in a debugger dialog.
export WINEDLLOVERRIDES="mscoree=;mshtml=;winemenubuilder.exe=;winedbg.exe="

Xvfb ":$display" -screen 0 1280x800x24 -nolisten tcp >/dev/null 2>&1 &
xvfb_pid=$!
for _ in $(seq 50); do
    [[ -e /tmp/.X11-unix/X$display ]] && break
    sleep 0.1
done
[[ -e /tmp/.X11-unix/X$display ]] || { echo "Xvfb did not start on :$display" >&2; exit 2; }

if [[ $keep_prefix -eq 0 || ! -f "$WINEPREFIX/system.reg" ]]; then
    echo "# creating Wine prefix (first run takes a while)..."
    timeout --kill-after=10 300 wineboot --init >/dev/null 2>&1 || true
    wineserver -w >/dev/null 2>&1 || true
fi

rc=0
for exe in "$@"; do
    echo "# wine $(basename "$exe") on :$display"
    status=0
    # timeout wraps wine directly: it must be the one receiving the signal.
    timeout --kill-after=5 "${PLAT_WINE_TIMEOUT:-120}" wine "$exe" || status=$?
    if [[ $status -ne 0 ]]; then
        echo "# $(basename "$exe") exited with status $status"
        [[ $rc -eq 0 ]] && rc=$status
    fi
done
exit $rc
