#!/usr/bin/env bash
set -e
# Build all Docker images.
# --network=host is required at build time so apt and git can reach the internet.
# Base images are built only if they don't exist yet. To force a rebuild, run
# the individual build_*_image.sh scripts explicitly.
if ! docker image inspect mate-slang:latest >/dev/null 2>&1; then
    bash "$(dirname "$0")/build_slang_image.sh"
fi
if ! docker image inspect mate-verilator:latest >/dev/null 2>&1; then
    bash "$(dirname "$0")/build_verilator_image.sh"
fi
docker build --network=host -t mate-dev .
#docker build --network=host -t mate-dev:claude -f Dockerfile.claude .

# Extract slang from the image into external/slang-install for host-side LSP.
CID=$(docker create mate-dev:latest)
docker cp "$CID":/opt/slang/. external/slang-install/
docker rm "$CID"
