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
- `python3 tools/dfg_inspect.py <file> cone <name> [--depth N]`
- `python3 tools/dfg_inspect.py <file> mux <name>`
- `python3 tools/dfg_inspect.py <file> mux-case <id> <case_val>` — look up one arm of a multi-way MUX by selector value (hex ok: `0x7C4`)
- `python3 tools/dfg_inspect.py <file> diff <older.json>`
- `python3 tools/dfg_inspect.py <file> op MUX`
- `python3 tools/dfg_inspect.py <file> node <name_or_id>`
- `python3 tools/dfg_inspect.py <file> const_driven 0` — list all SIGNAL/OUTPUT nodes driven by a given CONST value
- `python3 tools/dfg_inspect.py <file> group <prefix>` — compact table of all signals whose name starts with prefix (useful for struct fields split into per-signal nodes)
- `python3 tools/dfg_inspect.py <file> search <pattern>` — substring match when exact name is unknown

When debugging:
1. If symptoms suggest a whole struct is zeroed, run `const_driven 0` first — it surfaces all CONST-driven signals in one shot.
2. Use `group <struct_prefix>` to see all fields of a split struct and their drivers together.
3. Use `search <partial>` when the exact dot-notation name is unknown.
4. For a large MUX (e.g. CSR address decode), use `mux-case <id> <val>` instead of reading the full node output.
5. Use `cone <name> --depth N` to prune large cones to a readable depth.
6. Check `driver`, then `cone` or `mux` for the full picture.
7. Compare against the previous pass with `diff`.
8. If the issue is around a flop or domain tag, inspect `06_flop_resolve_flops.txt` and later pass outputs.
