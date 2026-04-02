#!/usr/bin/env bash
set -e
# Build both Docker images.
# --network=host is required at build time so apt and pip can reach the internet.
# Build the slang base image if it doesn't exist yet. To force a rebuild, run
# scripts/build_slang_image.sh explicitly.
if ! docker image inspect custom-hdl-slang:latest >/dev/null 2>&1; then
    bash "$(dirname "$0")/build_slang_image.sh"
fi
docker build --network=host -t custom-hdl-compiler .
docker build --network=host -t custom-hdl-compiler:claude -f Dockerfile.claude .
