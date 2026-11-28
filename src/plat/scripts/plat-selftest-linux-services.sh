#!/usr/bin/env bash
# Runs plat_selftest's desktop-service cases (tray, notifications, badge,
# network/sleep events, file dialogs, system settings) on both
# Linux backends against a PRIVATE session bus and a PRIVATE "system" bus
# with fake desktop services, then with the fallbacks (NetworkManager instead
# of the portal, a fake zenity instead of the portal FileChooser), then with
# no services and with no
# bus at all, and runs plat_services_probe for the portal settings, the
# activation tokens and bus-restart recovery.
# Never touches the user's session or system bus: the private dbus-daemons
# have no service directories, so nothing real gets activated on them either.
#
#   src/plat/scripts/plat-selftest-linux-services.sh <build-dir>
#
# Needs dbus-daemon, python3 with dbus-python and gi, Xvfb, and cage for the
# Wayland runs (skipped when missing). Exit status: first failure.
# (Internal: `--restart <tmpdir> <fake args…>` restarts the session bus and
# the fakes; the probe calls it.)
set -u

here=$(cd "$(dirname "$0")" && pwd)
fake=$here/fakes/plat_fake_desktop.py

# ── buses and fakes; pids live in files so --restart can replace them ───────
start_bus() { # name (bus | sysbus)
    local name=$1
    rm -f "$tmp/$name"
    dbus-daemon --config-file="$tmp/$name.conf" --fork --print-pid=1 >"$tmp/$name.pid" || return 1
    for _ in $(seq 100); do
        [ -S "$tmp/$name" ] && return 0
        sleep 0.02
    done
    return 1
}
stop_pidfile() {
    local f=$1 p
    [ -s "$f" ] || return 0
    p=$(cat "$f")
    kill "$p" 2>/dev/null
    for _ in $(seq 100); do
        kill -0 "$p" 2>/dev/null || break
        sleep 0.02
    done
    : >"$f"
}
start_fakes() {
    stop_pidfile "$tmp/fake.pid"
    : >"$tmp/fake.out"
    python3 "$fake" "$@" >"$tmp/fake.out" 2>>"$tmp/fake.log" &
    echo $! >"$tmp/fake.pid"
    for _ in $(seq 100); do
        grep -q ready "$tmp/fake.out" 2>/dev/null && return 0
        kill -0 "$(cat "$tmp/fake.pid")" 2>/dev/null || break
        sleep 0.05
    done
    echo "FAIL: fake desktop did not start"
    tail -n 20 "$tmp/fake.log" | sed 's/^/#   /'
    return 1
}
private_addresses() {
    # No guid in the addresses: a restarted daemon has a new one, and libdbus
    # refuses a server whose guid does not match.
    export DBUS_SESSION_BUS_ADDRESS="unix:path=$tmp/bus"
    export DBUS_SYSTEM_BUS_ADDRESS="unix:path=$tmp/sysbus"
    # Tell the X11/Wayland runners these are private and may be kept.
    export PLAT_SELFTEST_SESSION_BUS=1 PLAT_SELFTEST_SYSTEM_BUS=1
}

if [ "${1:-}" = "--restart" ]; then
    tmp=$2
    shift 2
    private_addresses
    stop_pidfile "$tmp/fake.pid"
    stop_pidfile "$tmp/bus.pid"
    start_bus bus || exit 1
    # Leave the fakes on the new bus running after we exit.
    start_fakes "$@" </dev/null || exit 1
    exit 0
fi

build=${1:?usage: $0 <build-dir>}
case $build in /*) ;; *) build=$(pwd)/$build ;; esac
selftest=$build/plat_selftest
# The standalone build puts the test binaries under tests/.
[ -x "$selftest" ] || selftest=$build/tests/plat_selftest
probe=$build/plat_services_probe
[ -x "$selftest" ] || { echo "FAIL: $selftest not built"; exit 2; }
command -v dbus-daemon >/dev/null || { echo "SKIP: dbus-daemon not installed"; exit 0; }
python3 -c 'import dbus, gi' 2>/dev/null || { echo "SKIP: python3-dbus/gi missing"; exit 0; }

tmp=$(mktemp -d "${TMPDIR:-/tmp}/plat-svc.XXXXXX") || exit 2
cleanup() {
    stop_pidfile "$tmp/fake.pid"
    stop_pidfile "$tmp/bus.pid"
    stop_pidfile "$tmp/sysbus.pid"
    rm -rf "$tmp"
}
trap cleanup EXIT INT TERM

for name in bus sysbus; do
    cat >"$tmp/$name.conf" <<EOF
<!DOCTYPE busconfig PUBLIC "-//freedesktop//DTD D-Bus Bus Configuration 1.0//EN"
 "http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd">
<busconfig>
  <type>session</type>
  <listen>unix:path=$tmp/$name</listen>
  <auth>EXTERNAL</auth>
  <policy context="default">
    <allow send_destination="*" eavesdrop="true"/>
    <allow eavesdrop="true"/>
    <allow own="*"/>
  </policy>
</busconfig>
EOF
done

start_bus bus || { echo "FAIL: private session dbus-daemon did not start"; exit 1; }
start_bus sysbus || { echo "FAIL: private system dbus-daemon did not start"; exit 1; }
private_addresses
echo "# private session bus $DBUS_SESSION_BUS_ADDRESS (pid $(cat "$tmp/bus.pid"))"
echo "# private system bus  $DBUS_SYSTEM_BUS_ADDRESS (pid $(cat "$tmp/sysbus.pid"))"

# The fallback file dialog: our fake as `zenity`, first on PATH.
mkdir -p "$tmp/bin"
ln -s "$here/fakes/plat_fake_dialog.py" "$tmp/bin/zenity"

rc=0
note() { [ "$1" -ne 0 ] && [ $rc -eq 0 ] && rc=$1; }

# Only the service cases (with the indented lines each case printed before
# its verdict) and the summary; the window cases are the backends' business.
filter() {
    awk '/^# backend/ || /^[0-9]+ passed/ { print; next }
         /^    / { buf = buf $0 "\n"; next }
         /^(PASS|FAIL|SKIP)/ {
             if ($0 ~ /badge|tray|notification|network|file dialog|system settings/)
                 printf "%s%s\n", buf, $0
             buf = ""
         }'
}

# The fake portal's settings are all non-default; this is how they must read.
wantSettings='reducedMotion 1 highContrast 1 textScale 1.25 accent ff336699 caretBlink 500 ms'
check_settings() { # log, label
    if grep -qF "$wantSettings" "$1"; then
        echo "PASS portal settings parsed ($2)"
    else
        echo "FAIL portal settings parsed ($2): want '$wantSettings'"
        note 1
    fi
}
# Every case in the list must PASS in the log.
require_pass() { # log, label, case-regex…
    local log=$1 label=$2
    shift 2
    for c in "$@"; do
        if ! grep -E "^PASS" "$log" | grep -qE "$c"; then
            echo "FAIL $label: '$c' did not pass"
            note 1
        fi
    done
}
dialogModes=('file dialog: save' 'file dialog: open one' 'file dialog: open several'
    'file dialog: pick a folder')
roundThree=('network / sleep' "${dialogModes[@]}" 'system settings')
# What the fake portal logged about each request's option types: the folder
# as a NUL-terminated ay, the name as s, the flags as b — per mode.
check_portal_options() { # label
    local log=$tmp/fake.log bad=0
    for want in \
        'portal-check: SaveFile multiple=none directory=none current_name=s current_folder=ay\+nul' \
        'portal-check: OpenFile multiple=none directory=none current_name=none current_folder=ay\+nul' \
        'portal-check: OpenFile multiple=b directory=none current_name=none current_folder=ay\+nul' \
        'portal-check: OpenFile multiple=none directory=b current_name=none current_folder=ay\+nul'; do
        grep -qE "$want" "$log" || { echo "FAIL portal options ($1): no '$want'"; bad=1; }
    done
    if grep -E 'portal-check:' "$log" | grep -qE 'wrong:|not-ay|bad-nul'; then
        echo "FAIL portal options ($1): badly typed option"
        grep -E 'portal-check:.*(wrong:|not-ay|bad-nul)' "$log" | sed 's/^/#   /'
        bad=1
    fi
    [ $bad -eq 0 ] && echo "PASS portal options per mode ($1)"
    note $bad
}

run_backends() { # label, [x11only]
    local label=$1 only=${2:-}
    echo "# ── $label: X11 (Xvfb) ──"
    "$here/plat-selftest-x11.sh" "$selftest" >"$tmp/x11.log" 2>&1
    note $?
    filter <"$tmp/x11.log"
    if [ -z "$only" ] && command -v cage >/dev/null; then
        echo "# ── $label: Wayland (cage) ──"
        "$here/plat-selftest-wayland.sh" "$selftest" cage >"$tmp/wl.log" 2>&1
        note $?
        filter <"$tmp/wl.log"
    else
        : >"$tmp/wl.log"
    fi
}

allFakes=(--watcher --notifications --launcher --portal --system)
start_fakes "${allFakes[@]}" || exit 1
: >"$tmp/fake.log"
run_backends "fake desktop"
check_portal_options "X11 + Wayland"
check_settings "$tmp/x11.log" X11
require_pass "$tmp/x11.log" "X11 + fakes" "${roundThree[@]}"
if [ -s "$tmp/wl.log" ]; then
    check_settings "$tmp/wl.log" Wayland
    require_pass "$tmp/wl.log" "Wayland + fakes" "${roundThree[@]}"
fi
# withdrawNotification reached the server: every "Withdrawn"/"Early" one was
# closed there (CloseNotification), never the "Kept" one.
check_withdrawn() {
    local log=$tmp/fake.log bad=0 n=0 id summary
    while read -r id summary; do
        n=$((n + 1))
        if [ "$summary" = Kept ]; then
            grep -qF "close: #$id " "$log" && { echo "FAIL withdraw: #$id (Kept) was closed"; bad=1; }
        elif ! grep -qF "close: #$id (open)" "$log"; then
            echo "FAIL withdraw: #$id ($summary) not closed on the server"
            bad=1
        fi
    done < <(sed -nE "s/.*notify: #([0-9]+) app=.* summary='(Withdrawn|Early|Kept)'.*/\1 \2/p" "$log")
    [ $n -eq 0 ] && { echo "FAIL withdraw: the fake saw none of the case's notifications"; bad=1; }
    [ $bad -eq 0 ] && echo "PASS withdrawn notifications closed on the server ($n seen)"
    note $bad
}
check_withdrawn
echo "# what the fakes saw:"
grep -E 'host:|notify:|close:|launcher:|portal: (Open|Save)|logind:|nm:|portal: network' \
    "$tmp/fake.log" | sed 's/^fake: /#   /' | head -n 70
takes=$(grep -c 'inhibitor taken' "$tmp/fake.log")
releases=$(grep -c 'inhibitor released' "$tmp/fake.log")
echo "# sleep inhibitors: $takes taken, $releases released"

echo "# ── fallbacks: NetworkManager, fake zenity (X11) ──"
start_fakes --watcher --notifications --launcher --portal --no-portal-network --no-file-chooser \
    --system || exit 1
: >"$tmp/fake.log"
PATH="$tmp/bin:$PATH" PLAT_FILE_DIALOG_FALLBACK=zenity \
    "$here/plat-selftest-x11.sh" "$selftest" >"$tmp/fb.log" 2>&1
note $?
filter <"$tmp/fb.log"
require_pass "$tmp/fb.log" "fallbacks" "${roundThree[@]}"
grep -E 'fallback dialog:|nm: state' "$tmp/fake.log" | sed 's/^fake: /#   /'

# showFileDialogEx without a FileChooser: Unavailable at once, and no helper
# tool unless PLAT_FILE_DIALOG_FALLBACK names one (the fake zenity is on PATH
# and must be started by the legacy call only).
echo "# ── no FileChooser, no fallback named: unavailable (X11) ──"
start_fakes --portal --no-file-chooser || exit 1
: >"$tmp/fake.log"
PATH="$tmp/bin:$PATH" PLAT_SELFTEST_FILE_DIALOG=unavailable \
    "$here/plat-selftest-x11.sh" "$selftest" >"$tmp/na.log" 2>&1
note $?
filter <"$tmp/na.log" | grep -E 'file dialog|^[0-9]+ passed|^    mode'
require_pass "$tmp/na.log" "no FileChooser" 'file dialog: unavailable'
spawned=$(grep -c 'fallback dialog: argv' "$tmp/fake.log")
if [ "$spawned" -eq 1 ]; then
    echo "PASS helper tool started by the legacy call only (1 run)"
else
    echo "FAIL helper tool runs: $spawned, want 1 (the legacy call)"
    note 1
fi

echo "# ── FileChooser whose backend fails (Response 2): unavailable (X11) ──"
start_fakes --portal --file-chooser-fails || exit 1
: >"$tmp/fake.log"
PLAT_FILE_DIALOG_FALLBACK=0 PLAT_SELFTEST_FILE_DIALOG=unavailable \
    "$here/plat-selftest-x11.sh" "$selftest" >"$tmp/fail.log" 2>&1
note $?
filter <"$tmp/fail.log" | grep -E 'file dialog|^[0-9]+ passed|^    mode'
require_pass "$tmp/fail.log" "failing FileChooser" 'file dialog: unavailable'
grep -c 'response 2' "$tmp/fake.log" | sed 's/^/# portal answered code 2 this many times: /'

echo "# ── FileChooser dialog closed by the user (late Response 2): cancelled (X11) ──"
start_fakes --portal --file-chooser-closed || exit 1
PLAT_FILE_DIALOG_FALLBACK=0 PLAT_SELFTEST_FILE_DIALOG=closed \
    "$here/plat-selftest-x11.sh" "$selftest" >"$tmp/closed.log" 2>&1
note $?
filter <"$tmp/closed.log" | grep -E 'file dialog|^[0-9]+ passed|^    mode'
require_pass "$tmp/closed.log" "closed FileChooser" 'file dialog: a closed dialog is a cancel'

if [ -x "$probe" ]; then
    start_fakes "${allFakes[@]}" || exit 1
    echo "# ── portal settings, activation tokens, bus restart (X11) ──"
    PLAT_PROBE_FULL=1 PLAT_PROBE_RESTART_CMD="$0 --restart $tmp ${allFakes[*]}" \
        "$here/plat-selftest-x11.sh" "$probe"
    note $?
    start_fakes --legacy-portal || exit 1
    echo "# ── v1 portal: Read, no ReadOne (X11) ──"
    "$here/plat-selftest-x11.sh" "$probe"
    note $?
fi

# Nothing on either bus. The real zenity must not pop up here.
stop_pidfile "$tmp/fake.pid"
export PLAT_FILE_DIALOG_FALLBACK=0 PLAT_SELFTEST_FILE_DIALOG=unavailable
start=$(date +%s)
run_backends "buses without desktop services"
echo "# (took $(($(date +%s) - start)) s)"
if [ -x "$probe" ]; then
    echo "# ── no services: the file dialog answers at once (X11) ──"
    PLAT_PROBE_NO_SERVICES=1 "$here/plat-selftest-x11.sh" "$probe"
    note $?
fi

echo "# ── no session or system bus at all: X11 ──"
env -u PLAT_SELFTEST_SYSTEM_BUS DBUS_SESSION_BUS_ADDRESS="unix:path=$tmp/no-such-bus" \
    "$here/plat-selftest-x11.sh" "$selftest" >"$tmp/nobus.log" 2>&1
note $?
filter <"$tmp/nobus.log"

exit $rc
