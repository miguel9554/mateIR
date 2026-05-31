---
name: read-vcd
description: Inspect VCD files from custom-sim or Verilator. Use this when debugging waveform values, transitions, or mismatches. Prefer tools/vcd_inspect.py.
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

Workflow:
1. Inspect `scopes`.
2. List `signals`.
3. Inspect `changes`.
4. Query `at`.
5. Cross-check the signal's driver with `read-dfg`.
