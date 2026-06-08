---
name: read-dfg
description: Inspect DFG .json or .dot debug artifacts produced by the compiler. Use this when debugging signal drivers, dependencies, fanout, op behavior, pass-to-pass changes, dead code, or node identity questions. Prefer tools/dfg_inspect.py and correlate with runtime tracing when structure alone is not enough.
---

Use `tools/dfg_inspect.py` for JSON and `dot -Tsvg` for DOT.

Common locations:
- `tests/<name>/work/static/debug_output/<top>/`
- `tests/<name>/work/custom-sim/debug_output/<top>/`

Key idea:
- Use DFG JSON for static structure and lowering state.
- Use `trace-dfg-runtime` when you need to know what happened at simulation time.

Useful commands:
- `python3 tools/dfg_inspect.py <file> node <id-or-name> [--details]`
- `python3 tools/dfg_inspect.py <file> nodes [--op OP] [--name PATTERN] [--at FILE:LINE]`
- `python3 tools/dfg_inspect.py <file> deps <id-or-name> [--op OP] [--depth N] [--details]`
- `python3 tools/dfg_inspect.py <file> uses <id-or-name> [--op OP] [--depth N] [--details]`
- `python3 tools/dfg_inspect.py <file> neighborhood <id-or-name> --fanin N --fanout N`
- `python3 tools/dfg_inspect.py <file> diff <other.json> [--node ID-OR-NAME] [--cone] [--details]`

Still useful when they fit:
- `python3 tools/dfg_inspect.py <file> driver <name>`
- `python3 tools/dfg_inspect.py <file> cone <name> [--depth N]`
- `python3 tools/dfg_inspect.py <file> mux <name>`
- `python3 tools/dfg_inspect.py <file> mux-case <id-or-name> <case_val>`
- `python3 tools/dfg_inspect.py <file> const_driven [value]`
- `python3 tools/dfg_inspect.py <file> group <prefix>`
- `python3 tools/dfg_inspect.py <file> search <pattern>`

When debugging:
1. Start with `node --details` for the suspicious signal or node.
2. Use `deps` to walk backward through the inputs that determine the value.
3. Use `uses` to see downstream impact and to find the first consumer that changes across passes.
4. Use `neighborhood` when the issue is local and you want one compact view.
5. Use `nodes --op OP` or `nodes --at FILE:LINE` when you know the operation kind or source location but not the exact name.
6. Use `diff` on adjacent passes first. Add `--node` or `--cone` once the suspect area is known.
7. If the static graph looks correct but the runtime value is wrong, switch to `trace-dfg-runtime`.

Notes:
- Newer DFG JSON carries richer type data, named inputs, `instance_path`, `full_name`, and stable `debug_id` values. Prefer those when present.
- Existing checked-in JSON artifacts may predate that schema. If metadata is missing, regenerate artifacts with a fresh compiler build.
