#!/usr/bin/env bash
set -e
docker run --network=host --rm -it \
  --cap-add=SYS_PTRACE \
  --user $(id -u):$(id -g) \
  -v "$(pwd)":/workspace \
  -v custom-hdl-compiler-build:/workspace/build \
  custom-hdl-compiler:latest \
  "${@:-bash}"
