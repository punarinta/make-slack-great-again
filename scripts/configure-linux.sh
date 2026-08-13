#!/usr/bin/env bash
# Install build dependencies for msga on Debian/Ubuntu.
set -euo pipefail

GREEN='\033[0;32m'; YELLOW='\033[1;33m'; RED='\033[0;31m'; NC='\033[0m'
ok()   { printf "  ${GREEN}ok${NC}    %s\n" "$*"; }
miss() { printf "  ${YELLOW}miss${NC}  %s\n" "$*"; }
# Each argument is one line: "$*" would join a multi-line explanation into one.
die()  { printf "  ${RED}error${NC} %s\n" "$1" >&2; shift; (($#)) && printf "        %s\n" "$@" >&2; exit 1; }

CMAKE_MIN_MAJOR=3
CMAKE_MIN_MINOR=21

QT_MIN_MAJOR=6
QT_MIN_MINOR=5

if ! command -v apt-get &>/dev/null; then
    die "This script requires apt (Debian/Ubuntu)." \
        "For other distros install the equivalent of: build-essential cmake git ninja-build" \
        "qt6-base-dev qt6-websockets-dev qt6-svg-dev libgl-dev"
fi

APT_PACKAGES=(
    build-essential   # g++ and make
    cmake
    git
    ninja-build
    qt6-base-dev      # Qt6::Core/Gui/Widgets/Network + moc/rcc
    qt6-websockets-dev
    qt6-svg-dev
    qt6-l10n-tools    # lupdate/lrelease binaries — compiles .ts translation files
    qt6-tools-dev     # Qt6LinguistTools CMake config — needed for find_package(Qt6 LinguistTools)
    libgl-dev         # OpenGL headers required by Qt Widgets
    clang-format      # formatting — matches .clang-format config
    clazy             # Qt-aware linting — provides clazy and clazy-standalone
    gcovr             # test coverage reports — used by scripts/coverage.sh
    xvfb              # xvfb-run — release-linux-static.sh startup smoke-launch
)

to_install=()
for pkg in "${APT_PACKAGES[@]%%#*}"; do  # strip inline comments
    [[ -z "$pkg" ]] && continue
    if dpkg -s "$pkg" &>/dev/null 2>&1; then
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

# cmake version check (apt may provide an outdated cmake on older distros)
cmake_ver=$(cmake --version | awk 'NR==1{print $3}')
cmake_maj=$(echo "$cmake_ver" | cut -d. -f1)
cmake_min=$(echo "$cmake_ver" | cut -d. -f2)
if [[ "$cmake_maj" -gt "$CMAKE_MIN_MAJOR" ]] || \
   [[ "$cmake_maj" -eq "$CMAKE_MIN_MAJOR" && "$cmake_min" -ge "$CMAKE_MIN_MINOR" ]]; then
    ok "cmake $cmake_ver (>= ${CMAKE_MIN_MAJOR}.${CMAKE_MIN_MINOR} required)"
else
    echo ""
    die "cmake $cmake_ver is too old — need >= ${CMAKE_MIN_MAJOR}.${CMAKE_MIN_MINOR}." \
        "Install a newer version from https://cmake.org/download/ or the Kitware APT PPA:" \
        "  https://apt.kitware.com/"
fi

# Qt version check. The distro packages installed above are frequently older
# than our floor (Ubuntu 24.04 LTS ships 6.4.2), and the resulting failure is a
# compile error deep into the build rather than anything that names Qt.
qmake_bin=""
if [[ -n "${QT_PREFIX:-}" && -x "${QT_PREFIX}/bin/qmake6" ]]; then
    qmake_bin="${QT_PREFIX}/bin/qmake6"
elif command -v qmake6 &>/dev/null; then
    qmake_bin="qmake6"
fi

if [[ -z "$qmake_bin" ]]; then
    miss "qmake6 not found — cannot verify the Qt version (need >= ${QT_MIN_MAJOR}.${QT_MIN_MINOR})"
else
    qt_ver=$("$qmake_bin" -query QT_VERSION)
    qt_maj=$(echo "$qt_ver" | cut -d. -f1)
    qt_min=$(echo "$qt_ver" | cut -d. -f2)
    if [[ "$qt_maj" -gt "$QT_MIN_MAJOR" ]] || \
       [[ "$qt_maj" -eq "$QT_MIN_MAJOR" && "$qt_min" -ge "$QT_MIN_MINOR" ]]; then
        ok "Qt $qt_ver (>= ${QT_MIN_MAJOR}.${QT_MIN_MINOR} required)"
    else
        echo ""
        die "Qt $qt_ver is too old — need >= ${QT_MIN_MAJOR}.${QT_MIN_MINOR} (QWebSocket::errorOccurred)." \
            "Your distro's Qt packages are behind. Install Qt with the online installer:" \
            "  https://www.qt.io/download-qt-installer" \
            "Tick the Qt WebSockets module (it is NOT selected by default), then build with:" \
            "  QT_PREFIX=\$HOME/Qt/<version>/gcc_64 ./scripts/build.sh"
    fi
fi

git config core.hooksPath .githooks
ok "git hooks (.githooks/pre-commit)"

echo ""
echo "All dependencies satisfied. Configure the project with:"
echo "  cmake -B build -S ."
