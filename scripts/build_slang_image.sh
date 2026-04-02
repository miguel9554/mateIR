#!/usr/bin/env bash
set -e
# Build the slang base image. Only needed once, or when the slang submodule is updated.
# --network=host is required at build time so apt can reach the internet.
docker build --network=host -t custom-hdl-slang:latest -f Dockerfile.slang .
