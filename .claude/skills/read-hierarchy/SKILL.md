---
name: read-hierarchy
description: Inspect a hierarchy.json file produced by the compiler. Use this when asked about module structure, ports, signals, flops, clock domains, or submodule relationships. Run scripts/hierarchy_inspect.py with the appropriate command.
argument-hint: <path/to/hierarchy.json> [command] [args]
---

Use `scripts/hierarchy_inspect.py` to inspect hierarchy JSON files.

## File location

Hierarchy files are written after each compiler run to:
```
<work-dir>/debug_output/<module_name>/hierarchy.json
```

For example:
```
tests/axi_spi_slave/work/custom-sim/debug_output/axi_spi_slave/hierarchy.json
```

## Schema

Each module node (root or submodule) has these fields:

| Field | Description |
|---|---|
| `name` | Module type name |
| `instance_name` | Instance name (empty string for the top module) |
| `pure_combinational` | True if the module has no flops |
| `inputs` / `outputs` / `signals` | Arrays of signal objects |
| `flops` | Array of flop objects |
| `parameters` / `localparams` | Array of parameter objects |
| `submodules` | Array of child module nodes (recursive) |

### Signal object

```json
{
  "name": "data",
  "type": {
    "kind": "integer",
    "width": 32,
    "signed": false,
    "packed_dims": [{"left": 31, "right": 0}],
    "unpacked_dims": []          // non-empty for array signals/ports
  },
  "sync_kind": "sync",           // clock | reset | sync | async
  "clock_domain": "clk",         // present when sync_kind == sync
  "clock_edge": "posedge"        // present for clock/reset/sync
}
```

### Flop object

```json
{
  "name": "counter",
  "type": { "width": 8, ... },
  "clock": {"edge": "posedge", "name": "clk"},
  "reset": {"edge": "negedge", "name": "rstn"},  // optional
  "reset_value": 0                               // optional
}
```

Array flops (e.g. `mem[7:0]`) are stored as individual elements: `mem[0]`, `mem[1]`, …, each as a scalar flop with no `unpacked_dims`. The names already encode the index.

## Available commands

```
python3 scripts/hierarchy_inspect.py <file> tree
python3 scripts/hierarchy_inspect.py <file> module  <instance.path>
python3 scripts/hierarchy_inspect.py <file> inputs  <instance.path>
python3 scripts/hierarchy_inspect.py <file> outputs <instance.path>
python3 scripts/hierarchy_inspect.py <file> signals <instance.path>
python3 scripts/hierarchy_inspect.py <file> flops   <instance.path>
python3 scripts/hierarchy_inspect.py <file> find    <signal_name>
python3 scripts/hierarchy_inspect.py <file> clocks
python3 scripts/hierarchy_inspect.py <file> stats
```

| Command | What it shows |
|---|---|
| `tree` | Indented module hierarchy with instance/type names and `(comb)` flag |
| `module <path>` | All ports, signals, flops, and submodules of one module |
| `inputs <path>` | Inputs with width, sync_kind, clock domain |
| `outputs <path>` | Outputs with width, sync_kind, clock domain |
| `signals <path>` | Internal signals |
| `flops <path>` | Flops with clock edge/name and reset edge/name/value |
| `find <name>` | Search for a signal or flop by exact name across the whole hierarchy |
| `clocks` | All distinct clock domains and which modules use each |
| `stats` | Per-module counts of inputs/outputs/signals/flops/submodules |

### Path format

The `<instance.path>` argument is dot-separated instance names from the root.
The leading root name is optional — both forms work:
```
axi_spi_slave.u_dcfifo_rx.u_din
u_dcfifo_rx.u_din
```
