#!/usr/bin/env bash
set -euo pipefail

# Stages the mate DPI toolchain into external/taxi/.mate-dist so the taxi
# cocotb flows (make MATE_DPI=1 / pytest with MATE_DPI=1) can invoke mate
# inside the taxi-test container, which mounts only the taxi tree.
#
# Re-run this after rebuilding the compiler (scripts/docker-run.sh make dev);
# the dist is a snapshot, not a link.

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$REPO_ROOT/build/dev}"
DIST="${MATE_DIST:-$REPO_ROOT/external/taxi/.mate-dist}"

for f in "$BUILD_DIR/mate" "$BUILD_DIR/libmate-abi-native.a"; do
    if [ ! -f "$f" ]; then
        echo "ERROR: missing $f — build first: scripts/docker-run.sh make dev" >&2
        exit 1
    fi
done

rm -rf "$DIST"
mkdir -p "$DIST/bin" "$DIST/lib" "$DIST/include/ieee1800"

cp "$BUILD_DIR/mate" "$DIST/bin/mate"
cp "$BUILD_DIR/libmate-abi-native.a" "$DIST/lib/libmate-abi-native.a"

# Headers the generated model/glue sources include (abi/, sim/, and their
# transitive includes) — copy the full src header tree, it is small.
(cd "$REPO_ROOT/src" && find . -name '*.h' -print0 | \
    xargs -0 -I{} install -D -m 644 "$REPO_ROOT/src/{}" "$DIST/include/mate-src/{}")
cp "$REPO_ROOT/external/slang/external/ieee1800/"*.h "$DIST/include/ieee1800/"

echo "Staged mate dist at $DIST"
