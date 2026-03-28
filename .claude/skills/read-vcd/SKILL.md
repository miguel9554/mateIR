---
name: read-vcd
description: Inspect VCD waveform files produced by the custom simulator or Verilator. Use this when debugging signal values, checking transitions, or understanding simulation output. Run scripts/vcd_inspect.py with the appropriate command.
argument-hint: <path/to/file.vcd> [command] [args]
---

Use `scripts/vcd_inspect.py` to inspect VCD files without loading them entirely into memory.

## File locations

Custom simulator output (grouped and raw):
```
tests/<module>/work/custom-sim/output/<module>.vcd       # grouped by hierarchy
tests/<module>/work/custom-sim/output/<module>-raw.vcd   # flat, one scope per module
```

Verilator reference output:
```
tests/<module>/work/verilator/waves.vcd
```

The `-raw.vcd` file is what `vcd-compare` uses for comparison. The grouped `.vcd` is for GTKWave.

## VCD format overview

A VCD file has two sections:

**Header** — parsed once, fast:
- `$timescale` — time unit (e.g. `1ns`)
- `$scope` / `$upscope` — module hierarchy
- `$var` — signal declarations: `$var wire <width> <id_code> <name> $end`
- `$enddefinitions $end` — end of header

**Value changes** — streamed:
- `#<timestamp>` — advance time
- `<0|1|x|z><id_code>` — scalar change (no space)
- `b<value> <id_code>` — vector change

Signal paths are written as `scope.name` using the full dot-separated hierarchy. Array elements appear as individual signals: `scope.mem[0]`, `scope.mem[1]`, etc.

## Available commands

```
python3 scripts/vcd_inspect.py <file> header
python3 scripts/vcd_inspect.py <file> scopes
python3 scripts/vcd_inspect.py <file> signals [scope_path]
python3 scripts/vcd_inspect.py <file> changes <signal_path>
python3 scripts/vcd_inspect.py <file> at      <signal_path> <time>
python3 scripts/vcd_inspect.py <file> stats
```

| Command | What it shows |
|---|---|
| `header` | Timescale, date, version from the VCD header |
| `scopes` | Module hierarchy as an indented tree with signal counts |
| `signals [scope]` | All signals (optionally filtered to a scope and its descendants) |
| `changes <path>` | All value transitions for one signal with timestamps |
| `at <path> <time>` | Value of a signal at a specific timestamp |
| `stats` | Total signals/scopes/timestamps, plus per-scope signal counts |

## Signal path format

Signal paths are `scope.signal_name` using the full dot-separated hierarchy exactly as written in the VCD:
```
axi_spi_slave.u_dcfifo_rx.u_din.mem[0]
axi_spi_slave.spi_sclk
```

Use `signals <scope>` to discover exact names when unsure.

## Workflow for debugging a wrong output value

1. `scopes` — confirm the scope hierarchy looks correct
2. `signals <scope>` — list signal names in the relevant module
3. `changes <signal>` — see every transition to find when the value goes wrong
4. `at <signal> <time>` — check the value at the exact timestamp of a mismatch
5. Cross-reference with the DFG using `read-dfg` to trace back to the driver
