---
name: debug-domains
description: Debug top-level domain YAML, CDC sidecars, top-domain inference, and synchronizer handling. Use this when working on .domains.yaml, .cdc.yaml, io_domains_set, cdc_annotations, global domain resolve, or domains_propagate_and_check.
---

Two sidecars:
- `.domains.yaml`: top-level clock, reset, sync, and async input classification
- `.cdc.yaml`: intentional synchronizer flops

Read:
- `src/domains.schema.json`
- `src/cdc.schema.json`

Relevant passes:
- `06_flop_resolve`
- `07_load_top_io_domains` or `07_infer_top_clock_reset_domains`
- `08_cdc_annotations`
- `09_global_domain_resolve`
- `10_infer_top_data_input_domains` in infer mode
- final `domains_propagate_and_check`

Key flags:
- `--domains`
- `--infer-top-domains`
- `--infer-synchronizers`
- `--emit-inferred-domains`

When debugging:
1. Inspect the sidecar schema.
2. Inspect `06_flop_resolve_flops.txt`.
3. Inspect pass outputs around 07-12.
4. Inspect `hierarchy.json`.
5. Check similar tests under `tests/`.
