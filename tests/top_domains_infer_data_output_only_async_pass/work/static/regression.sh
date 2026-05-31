#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")"

module_name=top_domains_infer_data_output_only_async_pass
make analyze INFER_TOP_DOMAINS=1 2>&1 | tee mate.log
grep -nF "top input 'passthrough_in' inferred async (sinks: none; synced-into: none)" mate.log
grep -nE '"name": "passthrough_in".*"sync_kind": "async"' "debug_output/$module_name/hierarchy.json"
