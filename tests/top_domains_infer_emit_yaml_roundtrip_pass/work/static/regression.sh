#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")"

module_name=top_domains_infer_emit_yaml_roundtrip_pass
emitted_yaml="$PWD/$module_name.generated.domains.yaml"
expected_yaml="../../rtl/$module_name.expected.domains.yaml"

rm -f "$emitted_yaml" infer.log roundtrip.log
make analyze INFER_TOP_DOMAINS=1 \
    EXTRA_ARGS="--emit-inferred-domains $emitted_yaml" 2>&1 | tee infer.log
diff -u "$expected_yaml" "$emitted_yaml"
make analyze EXTRA_ARGS="--domains $emitted_yaml" 2>&1 | tee roundtrip.log
