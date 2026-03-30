#!/usr/bin/env bash
set -e
docker run --network=host --rm -it \
  --cap-add=SYS_PTRACE \
  --user $(id -u):$(id -g) \
  -v "$(pwd)":/workspace \
  custom-hdl-compiler:latest \
  "${@:-bash}"
