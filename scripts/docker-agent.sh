#!/usr/bin/env bash
set -e

AGENT="${1:-claude}"
shift || true

case "$AGENT" in
  claude)
    IMAGE="custom-hdl-compiler:claude"
    EXTRA_VOLUMES="-v $HOME/.claude:$HOME/.claude -v $HOME/.claude.json:$HOME/.claude.json"
    ;;
  codex)
    IMAGE="custom-hdl-compiler:claude"
    EXTRA_VOLUMES="-v $HOME/.codex:$HOME/.codex"
    ;;
  *)
    echo "Usage: $0 [claude|codex] [args...]"
    exit 1
    ;;
esac

docker run --network=host --rm -it \
  --cap-add=SYS_PTRACE \
  --user $(id -u):$(id -g) \
  -v "$(pwd)":/workspace \
  -e HOST_PROJECT_ROOT="$(pwd)" \
  $EXTRA_VOLUMES \
  -e HOME=$HOME \
  "$IMAGE" \
  "${@:-bash}"
