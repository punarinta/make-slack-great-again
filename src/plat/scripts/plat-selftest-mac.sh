#!/usr/bin/env bash
# Build plat on the Mac build host and run its unit tests and the Cocoa
# selftest there, from the Linux host. The Mac user must be logged in at the
# console: the selftest opens real (short-lived) windows in that session.
#
#   src/plat/scripts/plat-selftest-mac.sh            # Debug build, bundled selftest
#   PLAT_MAC_HOST=user@host BUILD_TYPE=Release src/plat/scripts/plat-selftest-mac.sh
#   PLAT_MAC_BUNDLE=0 src/plat/scripts/plat-selftest-mac.sh   # bare executable
#
# By default the selftest runs from inside a minimal, ad-hoc signed .app
# (CFBundleIdentifier = the selftest's AppInfo id): UNUserNotificationCenter
# only serves an identified bundle, so that is the only way the notification
# case can run. PLAT_MAC_BUNDLE=0 runs the bare binary, which checks the
# unbundled path (notificationsAvailable() false, no exception).
#
# Exit status is the selftest's (non-zero on any FAIL or build error).
set -euo pipefail

host="${PLAT_MAC_HOST:-robin@192.168.32.249}"
remote_dir="${PLAT_MAC_DIR:-plat-verify}" # relative to the remote $HOME
build_type="${BUILD_TYPE:-Debug}"
bundle="${PLAT_MAC_BUNDLE:-1}"
here="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)" # plat/

ssh -o BatchMode=yes "$host" "mkdir -p '$remote_dir/plat'"
rsync -a --delete --exclude 'build*/' "$here/" "$host:$remote_dir/plat/"

# shellcheck disable=SC2087 # locals expand here, on purpose
ssh -o BatchMode=yes "$host" bash -s <<REMOTE
set -euo pipefail
export PATH="/opt/homebrew/bin:/usr/local/bin:\$PATH"
cd "$remote_dir"
gen=()
command -v ninja >/dev/null && gen=(-G Ninja)
# CMAKE_CXX_FLAGS= clears a stale '-include signal.h' an earlier stopgap left
# in the cache (selftest.cpp now includes <csignal> itself).
cmake -S plat -B build -DCMAKE_BUILD_TYPE=$build_type -DCMAKE_CXX_FLAGS= "\${gen[@]}" >/dev/null
cmake --build build
echo "== plat_unit_tests"
./build/plat_unit_tests

exe=./build/plat_selftest
if [ "$bundle" = 1 ]; then
    app=build/PlatSelftest.app
    rm -rf "\$app"
    mkdir -p "\$app/Contents/MacOS"
    cp build/plat_selftest "\$app/Contents/MacOS/"
    cat >"\$app/Contents/Info.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleIdentifier</key><string>org.nisdos.plat-selftest</string>
    <key>CFBundleName</key><string>plat selftest</string>
    <key>CFBundleExecutable</key><string>plat_selftest</string>
    <key>CFBundlePackageType</key><string>APPL</string>
    <key>CFBundleShortVersionString</key><string>1.0</string>
    <key>CFBundleVersion</key><string>1</string>
    <key>LSMinimumSystemVersion</key><string>11.0</string>
    <key>NSHighResolutionCapable</key><true/>
    <key>CFBundleURLTypes</key>
    <array><dict>
        <key>CFBundleURLName</key><string>org.nisdos.plat-selftest</string>
        <key>CFBundleURLSchemes</key><array><string>plat-selftest</string></array>
    </dict></array>
</dict>
</plist>
PLIST
    codesign --force --sign - "\$app" >/dev/null 2>&1
    # Launch Services must know the bundle before the notification center
    # will look its identifier up; a Finder launch would register it too.
    /System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister -f "\$app" || true
    exe="\$app/Contents/MacOS/plat_selftest"
fi

if [ "\$(ioreg -n Root -d1 -a | grep -A1 IOConsoleLocked | grep -c true)" != 0 ]; then
    echo "# note: the console session is locked (no key window, no real pointer)"
fi
# Scheme "registration" on macOS only reads the bundle's Info.plist (Launch
# Services learnt plat-selftest: from lsregister above), so the bundled
# selftest may exercise it without the throwaway HOME Linux needs.
[ "$bundle" = 1 ] && export PLAT_SELFTEST_ALLOW_REGISTRATION=1
echo "== plat_selftest (\$exe)"
# Bounded: a hung AppKit loop must not leave a window on the user's screen.
"\$exe" & pid=\$!
( sleep ${PLAT_MAC_TIMEOUT:-120}; kill -9 \$pid ) </dev/null >/dev/null 2>&1 & watchdog=\$!
status=0
wait \$pid || status=\$?
pkill -P \$watchdog 2>/dev/null || true # the sleep, so ssh need not wait for it
kill \$watchdog 2>/dev/null || true
exit \$status
REMOTE
