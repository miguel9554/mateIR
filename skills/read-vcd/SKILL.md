---
name: read-vcd
description: Inspect VCD files from custom-sim or Verilator. Use this when debugging waveform values, transitions, or mismatches. Prefer tools/vcd_inspect.py and the unified tools/vcd_diff.py.
---

Common files:
- `tests/<name>/work/custom-sim/output/<name>.vcd`
- `tests/<name>/work/custom-sim/output/<name>-raw.vcd`
- `tests/<name>/work/verilator/waves.vcd`

Useful commands:
- `python3 tools/vcd_inspect.py <file> header`
- `python3 tools/vcd_inspect.py <file> scopes`
- `python3 tools/vcd_inspect.py <file> signals [scope]`
- `python3 tools/vcd_inspect.py <file> changes <signal_path>`
- `python3 tools/vcd_inspect.py <file> at <signal_path> <time>`
- `python3 tools/vcd_inspect.py <file> stats`

Unified diff across two VCDs:
- exact signal pair: `python3 tools/vcd_diff.py signal <vcd1> <signal1> <vcd2> <signal2>`
- same suffix on both sides: `python3 tools/vcd_diff.py signal <vcd1> <vcd2> --signal <suffix>`
- compare matching direct-child signals in two scopes: `python3 tools/vcd_diff.py scope <vcd1> <scope1> <vcd2> <scope2> [--signals]`

Workflow:
1. Inspect `scopes`.
2. List `signals` in the relevant scope.
3. Use `vcd_diff.py signal` when you already know the suspicious signal.
4. Use `vcd_diff.py scope` when you need the first divergent field inside a bus-like or struct-like scope.
5. Once the first bad signal or time is known, cross-check with `read-dfg`.
6. If the DFG structure looks right but the value still diverges, switch to `trace-dfg-runtime`.
