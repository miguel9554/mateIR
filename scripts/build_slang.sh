#!/usr/bin/env bash
# Build and install slang into external/slang-install/.
# Run this once after cloning, or after updating the slang submodule.
# Never called from the main project build.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="$PROJECT_ROOT/build/slang-build"
INSTALL_DIR="$PROJECT_ROOT/external/slang-install"

echo "Building slang..."
cmake -B "$BUILD_DIR" -S "$PROJECT_ROOT/external/slang" \
    -DCMAKE_INSTALL_PREFIX="$INSTALL_DIR" \
    -DCMAKE_BUILD_TYPE=Release \
    -DSLANG_INCLUDE_TOOLS=OFF \
    -DSLANG_INCLUDE_TESTS=OFF \
    -DSLANG_INCLUDE_PYLIB=OFF \
    -DSLANG_INCLUDE_DOCS=OFF

cmake --build "$BUILD_DIR" -j"$(nproc)"

echo "Installing slang to $INSTALL_DIR..."
cmake --install "$BUILD_DIR"

echo "Done. slang installed to $INSTALL_DIR"
