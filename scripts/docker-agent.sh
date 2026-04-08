#!/usr/bin/env bash
set -e

AGENT="${1:-claude}"
shift || true

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
AGENT_IMAGE="custom-hdl-compiler:claude" exec "agent-sandbox" "$AGENT" "$@"
