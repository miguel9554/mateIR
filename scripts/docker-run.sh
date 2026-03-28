#!/usr/bin/env bash
set -e
BUILD_DIR="${XDG_CACHE_HOME:-$HOME/.cache}/custom-hdl-compiler/container-build"
mkdir -p "$BUILD_DIR"

docker run --network=host --rm -it \
  --cap-add=SYS_PTRACE \
  -v "$(pwd)":/workspace \
  -v "$BUILD_DIR":/workspace/build \
  custom-hdl-compiler:latest \
  "${@:-bash}"
