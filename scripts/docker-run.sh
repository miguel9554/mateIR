#!/usr/bin/env bash
set -e

docker_args=(
  --network=host
  --rm
)

if [[ "${DOCKER_LSP:-0}" == "1" ]]; then
  docker_args+=(-i)
elif [ -t 0 ] && [ -t 1 ]; then
  docker_args+=(-it)
fi

docker run "${docker_args[@]}" \
  --cap-add=SYS_PTRACE \
  --user "$(id -u):$(id -g)" \
  -v "$(pwd)":"$(pwd)" \
  -w "$(pwd)" \
  mate-dev:latest \
  "${@:-bash}"
