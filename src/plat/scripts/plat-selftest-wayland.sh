#!/bin/sh
# Runs plat_selftest against the Wayland backend inside private headless
# compositors. Builds nothing.
#
#   src/plat/scripts/plat-selftest-wayland.sh <path/to/plat_selftest> [cage|weston|sway|all]
#
# sway (wlroots, headless) is the full run: virtual keyboard/pointer,
# screencopy, primary selection, pointer gestures and floating windows. A
# generated config floats every window at the output origin, which is where
# the TestHooks assume a window is, and lets two windows show at once for
# drag and drop (the one created last stacks on top).
# cage (wlroots kiosk) makes every window fullscreen, so it runs without the
# two-window DnD cases. weston has no virtual input or screencopy (and no
# xdg-decoration), so it runs with PLAT_SELFTEST_NO_INPUT=1 to check the
# non-wlroots paths: configure, present, frame pacing, clipboard.
#
# Every compositor gets its own XDG_RUNTIME_DIR under a temp dir, so the
# user's session is never touched. Exit status is the selftest's (first
# failure wins when running several).
# PLAT_SELFTEST_LOGDIR=<dir> keeps each compositor's log there (sway with -d).
set -u

bin=${1:-}
which=${2:-all}
if [ -z "$bin" ] || [ ! -x "$bin" ]; then
    echo "usage: $0 <plat_selftest> [cage|weston|sway|all]" >&2
    exit 2
fi
case $bin in /*) ;; *) bin=$(pwd)/$bin ;; esac

# Never let the selftest's tray icon / notifications reach the user's real
# session bus: unless a caller (plat-selftest-linux-services.sh) hands us a
# private bus via PLAT_SELFTEST_SESSION_BUS, point D-Bus at nothing.
if [ -z "${PLAT_SELFTEST_SESSION_BUS:-}" ]; then
    userBus="unix:path=${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/bus"
    case ${DBUS_SESSION_BUS_ADDRESS:-} in
    "" | "$userBus" | "$userBus,"*) export DBUS_SESSION_BUS_ADDRESS="unix:path=/nonexistent/plat-selftest-bus" ;;
    esac
fi
# Same for the system bus (logind, NetworkManager): only a private one
# handed in via PLAT_SELFTEST_SYSTEM_BUS may be used.
if [ -z "${PLAT_SELFTEST_SYSTEM_BUS:-}" ]; then
    export DBUS_SYSTEM_BUS_ADDRESS="unix:path=/nonexistent/plat-selftest-system-bus"
fi

tmp=$(mktemp -d "${TMPDIR:-/tmp}/plat-wl.XXXXXX") || exit 2
pids=""
cleanup() {
    for p in $pids; do kill "$p" 2>/dev/null; done
    if [ -n "${PLAT_SELFTEST_LOGDIR:-}" ]; then
        mkdir -p "$PLAT_SELFTEST_LOGDIR" && cp "$tmp"/*/*.log "$PLAT_SELFTEST_LOGDIR"/ 2>/dev/null
    fi
    rm -rf "$tmp"
}
trap cleanup EXIT INT TERM

# A throwaway HOME and XDG base dirs: URL-scheme registration writes a
# .desktop handler and mimeapps.list, and nothing the selftest (or a
# compositor) writes may land in the user's real home. With that in place
# the selftest may register schemes (caseSingleInstance then checks OpenUrls).
# XDG_RUNTIME_DIR is private per compositor run below.
export HOME="$tmp/home"
export XDG_CONFIG_HOME="$HOME/.config" XDG_DATA_HOME="$HOME/.local/share"
export XDG_CACHE_HOME="$HOME/.cache" XDG_STATE_HOME="$HOME/.local/state"
mkdir -p "$XDG_CONFIG_HOME" "$XDG_DATA_HOME" "$XDG_CACHE_HOME" "$XDG_STATE_HOME" || exit 2
export PLAT_SELFTEST_ALLOW_REGISTRATION=1

# Scrub anything that could point the test at a real session.
unset WAYLAND_DISPLAY WAYLAND_SOCKET DISPLAY XDG_ACTIVATION_TOKEN SWAYSOCK
export PLAT_BACKEND=wayland

# Waits for a compositor to create its socket in $1; prints the socket name.
wait_socket() {
    i=0
    while [ $i -lt 100 ]; do
        for s in "$1"/wayland-* "$1"/plat-*; do
            case $s in *.lock) continue ;; esac
            if [ -S "$s" ]; then
                echo "${s##*/}"
                return 0
            fi
        done
        sleep 0.05
        i=$((i + 1))
    done
    return 1
}

run_cage() {
    if ! command -v cage >/dev/null 2>&1; then
        echo "# cage not installed: skipped"
        return 0
    fi
    rt=$tmp/cage
    mkdir -m 700 "$rt"
    echo "# ── cage (headless wlroots kiosk, fullscreen, no DnD) ──"
    # cage exits with its client, so its status is the selftest's.
    (
        cd "$rt" &&
        XDG_RUNTIME_DIR=$rt WLR_BACKENDS=headless WLR_RENDERER=pixman WLR_LIBINPUT_NO_DEVICES=1 \
            PLAT_SELFTEST_NO_DND=1 PLAT_SELFTEST_FULLSCREEN=1 \
            timeout 120 cage -- sh -c 'exec "$0"; ' "$bin" 2>"$rt/cage.log"
    )
    status=$?
    if [ $status -ne 0 ] && [ -s "$rt/cage.log" ]; then
        echo "# cage log (tail):"
        tail -n 20 "$rt/cage.log" | sed 's/^/#   /'
    fi
    return $status
}

run_sway() {
    if ! command -v sway >/dev/null 2>&1; then
        echo "# sway not installed: skipped"
        return 0
    fi
    rt=$tmp/sway
    mkdir -m 700 "$rt"
    echo "# ── sway (headless wlroots, floating windows at the origin) ──"
    # No bar block, so sway starts no swaybar. Borders off: the TestHooks
    # assume the surface's (0,0) is the output's.
    cat >"$rt/config" <<'EOF'
output HEADLESS-1 resolution 1280x720 position 0 0
default_border none
default_floating_border none
focus_follows_mouse no
for_window [title=".*"] floating enable, move position 0 0
for_window [title="^plat selftest$"] floating disable
gaps inner 0
gaps outer 0
gaps right 880
gaps bottom 420
EOF
    XDG_RUNTIME_DIR=$rt WLR_BACKENDS=headless WLR_RENDERER=pixman WLR_LIBINPUT_NO_DEVICES=1 \
        sway -D noatomic ${PLAT_SELFTEST_LOGDIR:+-d} -c "$rt/config" >"$rt/sway.log" 2>&1 &
    spid=$!
    pids="$pids $spid"
    if ! sock=$(wait_socket "$rt"); then
        echo "FAIL sway did not start"
        tail -n 20 "$rt/sway.log" | sed 's/^/#   /'
        return 1
    fi
    if [ -n "${PLAT_SELFTEST_LOGDIR:-}" ] && command -v swaymsg >/dev/null 2>&1; then
        # Window geometry/focus changes as sway saw them, for debugging.
        sleep 0.2
        SWAYSOCK=$(ls "$rt"/sway-ipc.*.sock 2>/dev/null | head -n 1) \
            swaymsg -m -r -t subscribe '["window"]' >"$rt/sway-ipc.log" 2>&1 &
        pids="$pids $!"
    fi
    XDG_RUNTIME_DIR=$rt WAYLAND_DISPLAY=$sock timeout 120 "$bin"
    status=$?
    kill "$spid" 2>/dev/null
    wait "$spid" 2>/dev/null
    return $status
}

run_weston() {
    if ! command -v weston >/dev/null 2>&1; then
        echo "# weston not installed: skipped"
        return 0
    fi
    rt=$tmp/weston
    mkdir -m 700 "$rt"
    echo "# ── weston (headless, no input injection) ──"
    XDG_RUNTIME_DIR=$rt weston --backend=headless --socket=plat-weston --idle-time=0 \
        >"$rt/weston.log" 2>&1 &
    wpid=$!
    pids="$pids $wpid"
    if ! wait_socket "$rt" >/dev/null; then
        echo "FAIL weston did not start"
        tail -n 20 "$rt/weston.log" | sed 's/^/#   /'
        return 1
    fi
    XDG_RUNTIME_DIR=$rt WAYLAND_DISPLAY=plat-weston PLAT_SELFTEST_NO_INPUT=1 timeout 120 "$bin"
    status=$?
    kill "$wpid" 2>/dev/null
    wait "$wpid" 2>/dev/null
    return $status
}

rc=0
case $which in
cage) run_cage || rc=$? ;;
weston) run_weston || rc=$? ;;
sway) run_sway || rc=$? ;;
all)
    run_sway || rc=$?
    run_cage || { s=$?; [ $rc -eq 0 ] && rc=$s; }
    run_weston || { s=$?; [ $rc -eq 0 ] && rc=$s; }
    ;;
*) echo "unknown compositor: $which" >&2; exit 2 ;;
esac
exit $rc
