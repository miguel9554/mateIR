---
name: debug
description: Triage a failing or misbehaving compiler test. Use this at the start of any debug session when a test is failing, a signal has a wrong value, a simulation mismatch appears, or a pass starts regressing and you need to choose the right artifact and workflow first.
---

## Step 1 - identify the failure mode

| Symptom | Start here |
|---|---|
| custom-sim vs Verilator output mismatch | `read-vcd` - find the first diverging signal or scope, then use `trace-dfg-runtime` or `read-dfg` on that cone |
| Signal has wrong or unexpected value | `read-dfg` - `node --details`, then `deps`, `uses`, or `neighborhood` |
| Need runtime evidence for how an op evaluated | `trace-dfg-runtime` |
| Entire struct or group of signals is zero | `read-dfg` - `const_driven 0`, then `group` or `uses` |
| Flop missing, wrong domain tag, or wrong reset | `read-hierarchy` - `flops <path>`, then check `06_flop_resolve_flops.txt` |
| Module structure wrong (missing port, wrong hierarchy) | `read-hierarchy` - `tree`, then `module <path>` |
| Clock/reset domain error, CDC failure, domain propagation issue | `debug-domains` |
| Pass regression (worked before, broken now) | `read-dfg` - compare adjacent pass JSONs with `diff`; narrow to one node or cone when possible |

## Step 2 - locate the debug artifacts

Artifacts live under the work directory used to reproduce the failure:
- `tests/<name>/work/static/debug_output/<top>/`
- `tests/<name>/work/custom-sim/debug_output/<top>/`
- `tests/<name>/work/custom-sim/output/`

Key files:
- per-pass DFG: `NN_<pass_name>.json` and `.dot`
- `hierarchy.json`
- `06_flop_resolve_flops.txt`
- `11_domains_propagate_flops.txt` or `12_domains_propagate_flops.txt`
- `output/dfg_trace.jsonl` when runtime DFG tracing is enabled
- VCDs under `output/` or `verilator/`

## Step 3 - narrow the first bad point

When the symptom is a wrong value or wrong structure:
1. Find the first bad signal, scope, or pass.
2. Prefer the smallest artifact that exposes the problem: a single node before a full cone, a single scope before the entire VCD.
3. If static DFG shape looks right but the runtime value is wrong, switch to `trace-dfg-runtime`.

## Sub-skills

- `read-dfg` - static DFG artifact inspection with `node`, `nodes`, `deps`, `uses`, `neighborhood`, and structural `diff`
- `trace-dfg-runtime` - runtime DFG evaluation tracing with `--trace-dfg-node`, `--trace-dfg-cone`, and `--trace-dfg-op`
- `read-hierarchy` - `hierarchy.json` inspection (`tree`, `flops`, `find`, `clocks`)
- `read-vcd` - waveform inspection and unified cross-sim diff (`vcd_inspect.py`, `vcd_diff.py`)
- `debug-domains` - domain/CDC pass workflow and sidecar schema
