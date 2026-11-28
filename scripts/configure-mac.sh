#!/usr/bin/env bash
# Install what building msga needs on macOS (Xcode command line tools +
# Homebrew), then check it. Nothing else: the app links only the system
# frameworks, and FreeType/HarfBuzz are built by msga's own CMake.
#
# Usage:
#   scripts/configure-mac.sh
set -euo pipefail

GREEN='\033[0;32m'; YELLOW='\033[1;33m'; RED='\033[0;31m'; NC='\033[0m'
ok()   { printf "  ${GREEN}ok${NC}    %s\n" "$*"; }
miss() { printf "  ${YELLOW}miss${NC}  %s\n" "$*"; }
die()  { printf "  ${RED}error${NC} %s\n" "$*" >&2; exit 1; }

CMAKE_MIN_MAJOR=3
CMAKE_MIN_MINOR=21

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
# clang-format discovery + its version floor, shared with format.sh and the hook.
source "${SCRIPT_DIR}/clang-format-env.sh"

[[ "$(uname -s)" == "Darwin" ]] || die "This script is for macOS (Linux: configure-linux.sh)."

# Xcode command line tools: clang, the SDK, git, python3, codesign, hdiutil.
if xcode-select -p &>/dev/null; then
    ok "Xcode command line tools ($(xcode-select -p))"
else
    miss "Xcode command line tools"
    echo ""
    echo "Launching the installer — run this script again once it has finished."
    xcode-select --install
    exit 0
fi

if command -v brew &>/dev/null; then
    ok "Homebrew ($(brew --version | head -1))"
else
    miss "Homebrew"
    echo ""
    echo "Installing Homebrew..."
    /bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
    # Apple Silicon installs to /opt/homebrew, which isn't on PATH yet.
    if [[ -x /opt/homebrew/bin/brew ]]; then
        eval "$(/opt/homebrew/bin/brew shellenv)"
    fi
fi

BREW_PACKAGES=(cmake ninja)

to_install=()
to_upgrade=()
for pkg in "${BREW_PACKAGES[@]}"; do
    if brew list "$pkg" &>/dev/null; then
        ok "$pkg ($(brew list --versions "$pkg" | awk '{print $2}'))"
    else
        miss "$pkg"
        to_install+=("$pkg")
    fi
done

# clang-format: Xcode's toolchain does not ship one, and the tree is formatted
# with >= ${MSGA_CLANG_FORMAT_MIN} — older releases produce DIFFERENT output from
# the same .clang-format (see clang-format-env.sh), so an old brew install must
# be upgraded, not merely present.
if msga_resolve_clang_format; then
    ok "clang-format (${MSGA_CLANG_FORMAT_VERSION}, >= ${MSGA_CLANG_FORMAT_MIN} required)"
elif brew list clang-format &>/dev/null; then
    miss "clang-format too old (${MSGA_CLANG_FORMAT_VERSION}) — need >= ${MSGA_CLANG_FORMAT_MIN}"
    to_upgrade+=(clang-format)
else
    miss "clang-format (>= ${MSGA_CLANG_FORMAT_MIN})"
    to_install+=(clang-format)
fi

if [[ ${#to_install[@]} -gt 0 ]]; then
    echo ""
    echo "Installing: ${to_install[*]}"
    brew install "${to_install[@]}"
fi
if [[ ${#to_upgrade[@]} -gt 0 ]]; then
    echo ""
    echo "Upgrading: ${to_upgrade[*]}"
    brew upgrade "${to_upgrade[@]}"
fi

cmake_ver=$(cmake --version | awk 'NR==1{print $3}')
cmake_maj=$(echo "$cmake_ver" | cut -d. -f1)
cmake_min=$(echo "$cmake_ver" | cut -d. -f2)
if [[ "$cmake_maj" -gt "$CMAKE_MIN_MAJOR" ]] ||
    [[ "$cmake_maj" -eq "$CMAKE_MIN_MAJOR" && "$cmake_min" -ge "$CMAKE_MIN_MINOR" ]]; then
    ok "cmake $cmake_ver (>= ${CMAKE_MIN_MAJOR}.${CMAKE_MIN_MINOR} required)"
else
    die "cmake $cmake_ver is too old — need >= ${CMAKE_MIN_MAJOR}.${CMAKE_MIN_MINOR}. Run: brew upgrade cmake"
fi
if msga_resolve_clang_format; then
    ok "clang-format (${MSGA_CLANG_FORMAT_VERSION})"
else
    miss "clang-format >= ${MSGA_CLANG_FORMAT_MIN} still not on PATH — run: brew install clang-format"
fi

if [[ -d "${PROJECT_ROOT}/.git" || -f "${PROJECT_ROOT}/.git" ]]; then
    git -C "$PROJECT_ROOT" config core.hooksPath .githooks
    ok "git hooks (.githooks/pre-commit)"
fi
if [[ ! -f "${PROJECT_ROOT}/credentials.cmake" ]]; then
    miss "credentials.cmake — builds work without it, but sign-in needs the Slack app keys"
    echo "        (copy credentials.cmake.example and fill it in)"
fi

echo ""
echo "Done. Build and test with:"
echo "  scripts/build.sh --test"
echo "Release (DMG into dist/):"
echo "  scripts/release-mac.sh"
