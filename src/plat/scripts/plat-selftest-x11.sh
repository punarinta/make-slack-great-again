#!/usr/bin/env bash
# Runs plat_selftest (or any plat test binary) against the X11 backend on a
# private Xvfb, never the user's display.
#
#   plat-selftest-x11.sh path/to/plat_selftest [bare|openbox|scale|all] [xvfb args…]
#
#   bare     no window manager (the default): our own focus/stacking paths
#   openbox  under openbox, a real reparenting EWMH window manager
#   scale    bare with PLAT_SCALE=1.5 (fractional scale)
#   all      bare, scale, openbox in turn; exit status is the first failure
#
# Extra Xvfb args replace the default screen (e.g. "-screen 0 1280x800x24").
# Env passes through, so PLAT_SCALE=1.5 or PLAT_X11_NO_SHM=1 work as usual.
# The user's own session bus is swapped for a dead address so the tray and
# notification cases can't reach the real desktop; a caller-provided private
# bus (plat-selftest-linux-services.sh) is kept. PLAT_SELFTEST_SESSION_BUS=1
# keeps whatever bus is set. HOME, XDG_*_HOME and XDG_RUNTIME_DIR always point
# into a temp dir, so URL-scheme registration never reaches the real ones.
set -u
selftest=${1:?usage: $0 path/to/plat_selftest [bare|openbox|scale|all] [xvfb args...]}
shift
mode=bare
case ${1:-} in
bare | openbox | scale | all)
    mode=$1
    shift
    ;;
esac
xvfbArgs=("$@")
[ ${#xvfbArgs[@]} -eq 0 ] && xvfbArgs=(-screen 0 1280x800x24)

command -v Xvfb >/dev/null || { echo "SKIP: Xvfb not installed"; exit 0; }

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

tmp=$(mktemp -d "${TMPDIR:-/tmp}/plat-x11.XXXXXX") || exit 2

# A throwaway HOME and XDG tree: the single-instance case registers a URL
# scheme (a .desktop file + mimeapps.list entry) and must never touch the
# user's own. user-dirs.dirs sets Downloads and disables Desktop, so the
# standard-dirs case sees both paths of the parser.
fakeHome="$tmp/home"
mkdir -p "$fakeHome/.config" "$fakeHome/.local/share" "$fakeHome/.cache" \
    "$fakeHome/.local/state" "$fakeHome/Downloads" "$tmp/runtime"
chmod 700 "$tmp/runtime"
cat >"$fakeHome/.config/user-dirs.dirs" <<'EOF'
XDG_DESKTOP_DIR="$HOME"
XDG_DOWNLOAD_DIR="$HOME/Downloads"
EOF
export HOME="$fakeHome" XDG_CONFIG_HOME="$fakeHome/.config" XDG_DATA_HOME="$fakeHome/.local/share" \
    XDG_CACHE_HOME="$fakeHome/.cache" XDG_STATE_HOME="$fakeHome/.local/state" \
    XDG_RUNTIME_DIR="$tmp/runtime" PLAT_SELFTEST_ALLOW_REGISTRATION=1
unset XDG_ACTIVATION_TOKEN DESKTOP_STARTUP_ID
xvfb=""
wm=""
stop() {
    [ -n "$wm" ] && { kill "$wm" 2>/dev/null; wait "$wm" 2>/dev/null; }
    [ -n "$xvfb" ] && { kill "$xvfb" 2>/dev/null; wait "$xvfb" 2>/dev/null; }
    wm=""
    xvfb=""
}
trap 'stop; rm -rf "$tmp"' EXIT INT TERM

# First free display number from 90 up (socket and lock file both absent).
start_xvfb() {
    disp=90
    while [ -e /tmp/.X11-unix/X$disp ] || [ -e /tmp/.X$disp-lock ]; do
        disp=$((disp + 1))
    done
    Xvfb :$disp -nolisten tcp +extension MIT-SHM "${xvfbArgs[@]}" >/dev/null 2>&1 &
    xvfb=$!
    for _ in $(seq 100); do
        [ -e /tmp/.X11-unix/X$disp ] && return 0
        kill -0 $xvfb 2>/dev/null || break
        sleep 0.05
    done
    echo "FAIL: Xvfb :$disp did not start"
    return 1
}

# openbox with a config of its own (not the user's ~/.config/openbox): new
# windows get focus, Smart placement keeps them from overlapping — the
# drag-and-drop cases aim at each window wherever it lands, they only need
# both visible.
start_openbox() {
    command -v openbox >/dev/null || { echo "SKIP: openbox not installed"; return 2; }
    cat >"$tmp/rc.xml" <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<openbox_config xmlns="http://openbox.org/3.4/rc">
  <focus><focusNew>yes</focusNew><followMouse>no</followMouse></focus>
  <placement><policy>Smart</policy><center>no</center></placement>
  <desktops><number>1</number></desktops>
</openbox_config>
EOF
    env -u WAYLAND_DISPLAY DISPLAY=:$disp HOME="$tmp" XDG_CONFIG_HOME="$tmp" \
        openbox --sm-disable --config-file "$tmp/rc.xml" >"$tmp/openbox.log" 2>&1 &
    wm=$!
    # Ready once it advertises itself (EWMH supporting-WM check window).
    for _ in $(seq 100); do
        if command -v xprop >/dev/null; then
            DISPLAY=:$disp xprop -root _NET_SUPPORTING_WM_CHECK 2>/dev/null | grep -q 'window id' && return 0
        fi
        kill -0 $wm 2>/dev/null || break
        sleep 0.05
    done
    command -v xprop >/dev/null && kill -0 $wm 2>/dev/null && return 0 # no xprop: assume ready after 5 s
    echo "FAIL: openbox did not start"
    sed 's/^/#   /' "$tmp/openbox.log" | tail -n 10
    return 1
}

run_one() { # label, env assignments…
    local label=$1
    shift
    start_xvfb || return 1
    if [ "$label" = openbox ]; then
        start_openbox
        local s=$?
        [ $s -ne 0 ] && { stop; [ $s -eq 2 ] && return 0; return 1; }
    fi
    [ "$mode" = all ] && echo "# ── X11 $label ──"
    env -u WAYLAND_DISPLAY DISPLAY=:$disp PLAT_BACKEND=x11 "$@" "$selftest"
    local rc=$?
    stop
    return $rc
}

case $mode in
bare) run_one bare ;;
scale) run_one scale PLAT_SCALE=1.5 ;;
openbox) run_one openbox ;;
all)
    rc=0
    run_one bare || [ $rc -ne 0 ] || rc=$?
    run_one scale PLAT_SCALE=1.5 || [ $rc -ne 0 ] || rc=$?
    run_one openbox || [ $rc -ne 0 ] || rc=$?
    exit $rc
    ;;
esac
