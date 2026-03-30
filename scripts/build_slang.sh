#!/usr/bin/env bash
# Build and install slang.
# Install prefix is controlled by SLANG_INSTALL_PREFIX. If unset, the default
# is chosen from CMAKE_BUILD_TYPE:
#   Release / RelWithDebInfo / MinSizeRel -> external/slang-install
#   Debug                                 -> external/slang-install-debug
# Run this once after cloning, or after updating the slang submodule.
# The project Makefile also uses this script to ensure a matching Debug slang
# is available before configuring a true Debug build.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
BUILD_TYPE="${CMAKE_BUILD_TYPE:-Release}"

case "$BUILD_TYPE" in
    Debug)
        DEFAULT_INSTALL_DIR="$PROJECT_ROOT/external/slang-install-debug"
        ;;
    Release|RelWithDebInfo|MinSizeRel)
        DEFAULT_INSTALL_DIR="$PROJECT_ROOT/external/slang-install"
        ;;
    *)
        echo "Unsupported CMAKE_BUILD_TYPE '$BUILD_TYPE'" >&2
        exit 1
        ;;
esac

BUILD_DIR="$PROJECT_ROOT/build/slang-build-$BUILD_TYPE"
INSTALL_DIR="${SLANG_INSTALL_PREFIX:-$DEFAULT_INSTALL_DIR}"

echo "Building slang ($BUILD_TYPE)..."
cmake -B "$BUILD_DIR" -S "$PROJECT_ROOT/external/slang" \
    -DCMAKE_INSTALL_PREFIX="$INSTALL_DIR" \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
    -DSLANG_INCLUDE_TOOLS=OFF \
    -DSLANG_INCLUDE_TESTS=OFF \
    -DSLANG_INCLUDE_PYLIB=OFF \
    -DSLANG_INCLUDE_DOCS=OFF \
    -DSLANG_USE_MIMALLOC=OFF

cmake --build "$BUILD_DIR" -j"$(nproc)"

echo "Installing slang to $INSTALL_DIR..."
cmake --install "$BUILD_DIR"

echo "Done. slang installed to $INSTALL_DIR"
