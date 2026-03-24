#!/usr/bin/env bash
set -e
# Build the Docker image.
# --network=host is required at build time so apt and pip can reach the internet.
docker build --network=host -t custom-hdl-compiler .
