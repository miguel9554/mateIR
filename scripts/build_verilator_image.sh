#!/usr/bin/env bash
set -e
# Build the Verilator base image. Only needed once, or when the Verilator version changes.
# --network=host is required at build time so apt and git can reach the internet.
docker build --network=host -t mate-verilator:latest -f Dockerfile.verilator .
