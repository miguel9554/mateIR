#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")"
make analyze INFER_TOP_DOMAINS=1
