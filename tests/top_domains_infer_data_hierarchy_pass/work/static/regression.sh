#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")"

module_name=top_domains_infer_data_hierarchy_pass
make analyze INFER_TOP_DOMAINS=1 2>&1 | tee mate.log
grep -nE '"name": "din".*"sync_kind": "sync"' "debug_output/$module_name/hierarchy.json"
