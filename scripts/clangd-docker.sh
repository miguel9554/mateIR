#!/bin/bash
DOCKER_LSP=1 exec "$(dirname "$0")/docker-run.sh" clangd-18 --log=error "$@"
