#!/usr/bin/env bash
set -e
docker run --network=host --rm -it \
  --cap-add=SYS_PTRACE \
  --user $(id -u):$(id -g) \
  -v "$(pwd)":/workspace \
  -v "$HOME/.claude":"$HOME/.claude" \
  -v "$HOME/.claude.json":"$HOME/.claude.json" \
  -e ANTHROPIC_API_KEY=$ANTHROPIC_API_KEY \
  -e HOME=$HOME \
  custom-hdl-compiler:claude \
  "${@:-bash}"
