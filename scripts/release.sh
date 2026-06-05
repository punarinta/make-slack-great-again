#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${PROJECT_ROOT}/build-release"
NPROC="$(nproc)"

# Always reconfigure release builds to ensure optimisation flags are applied.
cmake -S "$PROJECT_ROOT" -B "$BUILD_DIR" \
    -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_CXX_FLAGS_RELEASE="-O3 -DNDEBUG" \
    -DCMAKE_INTERPROCEDURAL_OPTIMIZATION=ON

cmake --build "$BUILD_DIR" --target msga --parallel "$NPROC"

strip --strip-all "${BUILD_DIR}/msga"

echo ""
echo "Binary: ${BUILD_DIR}/msga"
ls -lh "${BUILD_DIR}/msga"
