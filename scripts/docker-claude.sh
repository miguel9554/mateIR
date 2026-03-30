#!/usr/bin/env bash
set -e

AGENT="${1:-claude}"
shift || true

case "$AGENT" in
  claude)
    IMAGE="custom-hdl-compiler:claude"
    EXTRA_VOLUMES="-v $HOME/.claude:$HOME/.claude -v $HOME/.claude.json:$HOME/.claude.json"
    API_KEY_ENV="-e ANTHROPIC_API_KEY=$ANTHROPIC_API_KEY"
    ;;
  codex)
    IMAGE="custom-hdl-compiler:claude"
    EXTRA_VOLUMES="-v $HOME/.codex:$HOME/.codex"
    API_KEY_ENV="-e OPENAI_API_KEY=$OPENAI_API_KEY"
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
  $EXTRA_VOLUMES \
  $API_KEY_ENV \
  -e HOME=$HOME \
  "$IMAGE" \
  "${@:-bash}"
