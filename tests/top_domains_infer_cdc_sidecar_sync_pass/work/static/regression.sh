#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")"

module_name=top_domains_infer_cdc_sidecar_sync_pass
make analyze INFER_TOP_DOMAINS=1 2>&1 | tee mate.log
grep -nF "top input 'async_in' inferred async (sinks: none; synced-into: clk@meta)" mate.log
grep -nE '"name": "async_in".*"sync_kind": "async"' "debug_output/$module_name/hierarchy.json"
