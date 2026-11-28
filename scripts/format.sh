#!/usr/bin/env bash
# Run clang-format -i on all C++ source files (or a specific list passed as args).
# Usage:
#   scripts/format.sh          # format the entire src/ and tests/ trees
#   scripts/format.sh f1 f2    # format specific files
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$SCRIPT_DIR/.."

# The tree is formatted with clang-format >= 22; older releases emit different
# output from the same config, so refuse rather than silently reformat the tree
# the other way (see clang-format-env.sh).
source "$SCRIPT_DIR/clang-format-env.sh"
if ! msga_resolve_clang_format; then
    if [[ -n "$CLANG_FORMAT" ]]; then
        echo "format.sh: $CLANG_FORMAT is too old (${MSGA_CLANG_FORMAT_VERSION})"
    else
        echo "format.sh: clang-format not found"
    fi
    msga_clang_format_hint | sed 's/^/  /'
    exit 1
fi

if [[ $# -gt 0 ]]; then
    files=("$@")
else
    mapfile -t files < <(find "$ROOT/src" "$ROOT/tests" -path "$ROOT/src/third_party" -prune -o \
        \( -name '*.cpp' -o -name '*.h' \) ! -name 'icons_generated.*' ! -name 'languages_generated.cpp' -print | sort)
fi

if [[ ${#files[@]} -eq 0 ]]; then
    echo "format.sh: no files found"
    exit 0
fi

"$CLANG_FORMAT" -i "${files[@]}"
echo "format.sh: formatted ${#files[@]} file(s) with ${MSGA_CLANG_FORMAT_VERSION}"
