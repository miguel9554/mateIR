---
name: read-vcd
description: Inspect VCD files from custom-sim or Verilator. Use this when debugging waveform values, transitions, or mismatches. Prefer tools/vcd_inspect.py and tools/vcd_diff_signal.py.
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

Signal diff across two VCDs (handles timescale mismatch):
- `python3 tools/vcd_diff_signal.py <vcd1> <signal1> <vcd2> <signal2>`

Workflow:
1. Inspect `scopes`.
2. List `signals`.
3. Inspect `changes`.
4. Query `at`.
5. To compare the same signal between custom-sim and Verilator, use `vcd_diff_signal.py`.
6. Cross-check the signal's driver with `read-dfg`.
