#!/usr/bin/env bash
# Install build dependencies for msga on macOS via Homebrew.
set -euo pipefail

GREEN='\033[0;32m'; YELLOW='\033[1;33m'; RED='\033[0;31m'; NC='\033[0m'
ok()   { printf "  ${GREEN}ok${NC}    %s\n" "$*"; }
miss() { printf "  ${YELLOW}miss${NC}  %s\n" "$*"; }
die()  { printf "  ${RED}error${NC} %s\n" "$*" >&2; exit 1; }

CMAKE_MIN_MAJOR=3
CMAKE_MIN_MINOR=21

# Xcode Command Line Tools (provides clang, git, make)
if xcode-select -p &>/dev/null 2>&1; then
    ok "Xcode Command Line Tools ($(xcode-select -p))"
else
    miss "Xcode Command Line Tools"
    echo ""
    echo "Launching installer — re-run this script once the installation completes."
    xcode-select --install
    exit 0
fi

# Homebrew
if command -v brew &>/dev/null; then
    ok "Homebrew ($(brew --version | head -1))"
else
    miss "Homebrew"
    echo ""
    echo "Installing Homebrew..."
    /bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
    # Add brew to PATH for the rest of this script (Apple Silicon installs to /opt/homebrew)
    if [[ -x /opt/homebrew/bin/brew ]]; then
        eval "$(/opt/homebrew/bin/brew shellenv)"
    fi
fi

BREW_PACKAGES=(
    cmake
    ninja
    qt     # Qt6 — includes Core/Gui/Widgets/Network/WebSockets/Svg and all other modules
    gcovr  # test coverage reports — used by scripts/coverage.sh
)

to_install=()
for pkg in "${BREW_PACKAGES[@]}"; do
    if brew list "$pkg" &>/dev/null 2>&1; then
        ok "$pkg ($(brew list --versions "$pkg" | awk '{print $2}'))"
    else
        miss "$pkg"
        to_install+=("$pkg")
    fi
done

if [[ ${#to_install[@]} -gt 0 ]]; then
    echo ""
    echo "Installing: ${to_install[*]}"
    brew install "${to_install[@]}"
fi

# cmake version check
cmake_ver=$(cmake --version | awk 'NR==1{print $3}')
cmake_maj=$(echo "$cmake_ver" | cut -d. -f1)
cmake_min=$(echo "$cmake_ver" | cut -d. -f2)
if [[ "$cmake_maj" -gt "$CMAKE_MIN_MAJOR" ]] || \
   [[ "$cmake_maj" -eq "$CMAKE_MIN_MAJOR" && "$cmake_min" -ge "$CMAKE_MIN_MINOR" ]]; then
    ok "cmake $cmake_ver (>= ${CMAKE_MIN_MAJOR}.${CMAKE_MIN_MINOR} required)"
else
    die "cmake $cmake_ver is too old — need >= ${CMAKE_MIN_MAJOR}.${CMAKE_MIN_MINOR}. Run: brew upgrade cmake"
fi

# Linting tools — clang-format ships with Xcode CLT; clazy requires brew.
if command -v clang-format &>/dev/null 2>&1; then
    ok "clang-format"
else
    miss "clang-format — install via: brew install clang-format"
fi
for tool in clazy clazy-standalone; do
    if command -v "$tool" &>/dev/null 2>&1; then
        ok "$tool"
    else
        miss "$tool — install via: brew install clazy"
    fi
done

# Qt is not on the default PATH because Homebrew intentionally leaves it keg-only
# to avoid shadowing macOS system frameworks. Pass its prefix to cmake explicitly.
QT_PREFIX=$(brew --prefix qt)

git config core.hooksPath .githooks
ok "git hooks (.githooks/pre-commit)"

echo ""
echo "All dependencies satisfied. Configure the project with:"
echo "  cmake -B build -S . -DCMAKE_PREFIX_PATH=${QT_PREFIX}"
