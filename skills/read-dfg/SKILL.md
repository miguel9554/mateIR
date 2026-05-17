---
name: read-dfg
description: Inspect DFG .json or .dot debug artifacts produced by the compiler. Use this when debugging signal drivers, cones, MUX trees, pass-to-pass changes, dead code, or node-path questions. Prefer tools/dfg_inspect.py and compiler debug-node flags over ad hoc scripts.
---

Use `tools/dfg_inspect.py` for JSON and `dot -Tsvg` for DOT.

Common locations:
- `tests/<name>/work/static/debug_output/<top>/`
- `tests/<name>/work/custom-sim/debug_output/<top>/`

Current pass order:
- `00_elaboration`
- `01_dfg_inline`
- `02_constant_fold`
- `03_type_propagation`
- `04_condition_normalization`
- `05_constant_fold`
- `06_flop_resolve`
- `07_load_top_io_domains` or `07_infer_top_clock_reset_domains`
- `08_cdc_annotations`
- `09_global_domain_resolve`
- `10_infer_top_data_input_domains` only in infer mode
- `10_dce` or `11_dce`
- `11_domains_propagate_and_check` or `12_domains_propagate_and_check`

Also emitted:
- `hierarchy.json`
- `06_flop_resolve_flops.txt`
- `11_domains_propagate_flops.txt` or `12_domains_propagate_flops.txt`

Useful commands:
- `python3 tools/dfg_inspect.py <file> driver <name>`
- `python3 tools/dfg_inspect.py <file> cone <name>`
- `python3 tools/dfg_inspect.py <file> mux <name>`
- `python3 tools/dfg_inspect.py <file> diff <older.json>`
- `python3 tools/dfg_inspect.py <file> op MUX`
- `python3 tools/dfg_inspect.py <file> node <name_or_id>`

When debugging:
1. Find the latest pass snapshot for the failing signal.
2. Check `driver`.
3. Check `cone` or `mux`.
4. Compare against the previous pass with `diff`.
5. If the issue is around a flop or domain tag, inspect `06_flop_resolve_flops.txt` and later pass outputs.
