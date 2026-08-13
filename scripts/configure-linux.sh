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

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# Qt discovery + the 6.5 floor live here, shared with the build scripts.
source "${SCRIPT_DIR}/qt-prefix.sh"

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

# Qt version + module check, shared with the build scripts so this reports
# exactly the Qt they will use. The distro packages installed above are
# frequently older than our floor (Ubuntu 24.04 LTS ships 6.4.2) and the
# resulting failure is a compile error deep into the build that never names Qt;
# a Qt installed without the WebSockets module fails the same way. Both die here
# with instructions instead. Resolution order: $QT_PREFIX, system Qt, then the
# newest ~/Qt kit that qualifies.
msga_resolve_qt_prefix

if [[ -n "${QT_PREFIX:-}" ]]; then
    ok "Qt ${MSGA_QT_VERSION:-?} at ${QT_PREFIX} (${MSGA_QT_SOURCE}, >= ${MSGA_QT_MIN} required)"
elif [[ -n "${MSGA_QT_VERSION:-}" ]]; then
    ok "Qt ${MSGA_QT_VERSION} (system, >= ${MSGA_QT_MIN} required)"
else
    miss "qmake6 not found — cannot verify the Qt version (need >= ${MSGA_QT_MIN})"
fi

git config core.hooksPath .githooks
ok "git hooks (.githooks/pre-commit)"

echo ""
echo "All dependencies satisfied. Build with:"
if [[ -n "${QT_PREFIX:-}" ]]; then
    # The build scripts re-run this same detection, so the export is only needed
    # when the kit sits somewhere they don't look.
    echo "  QT_PREFIX=${QT_PREFIX} ./scripts/build.sh"
else
    echo "  ./scripts/build.sh"
fi
