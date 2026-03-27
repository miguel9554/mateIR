#!/usr/bin/env bash
set -e

# Auto-configure cmake on first run (build/ named volume starts empty).
if [ ! -f build/CMakeCache.txt ]; then
    echo "[entrypoint] Configuring cmake..."
    cmake -B build -S . -DCMAKE_BUILD_TYPE=RelWithDebInfo
fi

exec "$@"
