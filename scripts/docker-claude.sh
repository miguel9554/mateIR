#!/usr/bin/env bash
docker run --network=host --rm -it \
  --cap-add=SYS_PTRACE \
  --user $(id -u):$(id -g) \
  -v "$(pwd)":/workspace \
  -v custom-hdl-compiler-build:/workspace/build \
  -v "$HOME/.claude":"$HOME/.claude" \
  -v "$HOME/.claude.json":"$HOME/.claude.json" \
  -e ANTHROPIC_API_KEY=$ANTHROPIC_API_KEY \
  -e HOME=$HOME \
  --user $(id -u):$(id -g) \
  custom-hdl-compiler:claude \
  "${@:-bash}"
