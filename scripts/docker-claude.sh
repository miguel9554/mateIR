#!/usr/bin/env bash
BUILD_DIR="${XDG_CACHE_HOME:-$HOME/.cache}/custom-hdl-compiler/container-build"
mkdir -p "$BUILD_DIR"
docker run --network=host --rm -it \
  --cap-add=SYS_PTRACE \
  -v "$(pwd)":/workspace \
  -v "$BUILD_DIR":/workspace/build \
  -v "$HOME/.claude":"$HOME/.claude" \
  -v "$HOME/.claude.json":"$HOME/.claude.json" \
  -e ANTHROPIC_API_KEY=$ANTHROPIC_API_KEY \
  -e HOME=$HOME \
  --user $(id -u):$(id -g) \
  custom-hdl-compiler:claude \
  "${@:-bash}"
