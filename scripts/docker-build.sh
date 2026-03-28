#!/usr/bin/env bash
set -e
# Build both Docker images.
# --network=host is required at build time so apt and pip can reach the internet.
docker build --network=host -t custom-hdl-compiler .
docker build --network=host -t custom-hdl-compiler:claude -f Dockerfile.claude .
