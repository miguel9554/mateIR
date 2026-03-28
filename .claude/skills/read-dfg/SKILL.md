---
name: read-dfg
description: Inspect DFG dot or JSON files produced by the compiler after each pass. Use this when debugging signal drivers, MUX trees, combinational cones, dead code, or comparing DFGs across passes. Run scripts/dfg_inspect.py with the appropriate command.
argument-hint: <path/to/dfg.json> [command] [args]
---

Use `scripts/dfg_inspect.py` to inspect DFG JSON files, and any `.dot` viewer (or `dot -Tsvg`) for `.dot` files.

## File location

After each compiler pass, debug output is written to:
```
debug_output/<module_name>/<N>_<pass_name>.json
debug_output/<module_name>/<N>_<pass_name>.dot
```

Pass numbering:
```
0_extractor
1_elaboration
2a_concat_cleanup
2b_type_propagation
3_condition_normalization
4_constant_fold
5_dce
6_flop_resolve
7_io_domains_set
8_combo_deps
```

Always use the latest-numbered file for the current state. Use an earlier file to debug what a specific pass changed — the `diff` command compares two JSON snapshots directly.

## DFG JSON schema

```json
{
  "nodes": [
    {
      "id":     42,
      "op":     "MUX",
      "name":   "out_signal",   // present on named nodes
      "value":  7,              // present on CONST nodes
      "inputs": [10, 20, 30],  // ids of input nodes (slot order)
      "type":   {"width": 8, "signed": false},
      "loc":    "foo.sv:12"
    }
  ]
}
```

### Node ops

| Op | Role | Input slots |
|---|---|---|
| `INPUT` | Module input or flop `.q` source | — |
| `OUTPUT` | Module output or flop `.d` sink | [0] driver |
| `SIGNAL` | Internal wire | [0] driver (or multiple for aggregate arrays) |
| `CONST` | Constant literal | — |
| `MUX` | Multiplexer | [0] sel, [1] true-branch, [2] false-branch |
| `INDEX` | Bit-select / slice | [0] src, [1] hi, [2] lo |
| `CONCAT` | Bit concatenation | [0..N] parts, MSB first |
| `NOT` | Bitwise NOT | [0] |
| `AND` / `OR` / `XOR` | Bitwise | [0], [1] |
| `ADD` / `SUB` / `MUL` | Arithmetic | [0], [1] |
| `EQ` / `NEQ` / `LT` / `LTE` / `GT` / `GTE` | Compare | [0], [1] |
| `SHL` / `SHR` / `ASHR` | Shift | [0] value, [1] amount |
| `ZEXT` / `SEXT` / `TRUNC` | Width cast | [0] |
| `REDUCE_OR` / `REDUCE_AND` / `REDUCE_XOR` | Unary reduction | [0] |

Flop nodes appear as named `INPUT` (`.q`) and named `OUTPUT` (`.d`). Clock and reset are **not** in the DFG.

## Available commands

```
python3 scripts/dfg_inspect.py <file> driver  <name_or_id>
python3 scripts/dfg_inspect.py <file> cone    <name_or_id> [max_depth]
python3 scripts/dfg_inspect.py <file> node    <name_or_id>
python3 scripts/dfg_inspect.py <file> op      <opname>
python3 scripts/dfg_inspect.py <file> fanout  <name_or_id>
python3 scripts/dfg_inspect.py <file> mux     <name_or_id>
python3 scripts/dfg_inspect.py <file> inputs
python3 scripts/dfg_inspect.py <file> outputs
python3 scripts/dfg_inspect.py <file> signals
python3 scripts/dfg_inspect.py <file> undriven
python3 scripts/dfg_inspect.py <file> diff    <file2>
python3 scripts/dfg_inspect.py <file> path    <id1> <id2>
```

| Command | What it shows |
|---|---|
| `driver <name>` | The immediate driver(s) of a named signal/output |
| `cone <name>` | Full backward transitive cone (all inputs that reach this node) |
| `node <id>` | Details of one node: op, type, inputs, fanout consumers |
| `op <opname>` | All nodes with a given op (e.g. `MUX`, `INDEX`, `CONCAT`) |
| `fanout <id>` | All nodes that consume a given node |
| `mux <name>` | The MUX tree rooted at the driver of a signal — shows sel/T/F recursively |
| `inputs` | All `INPUT` nodes with their types |
| `outputs` | All `OUTPUT` nodes and their drivers |
| `signals` | All `SIGNAL` nodes and their drivers |
| `undriven` | All signals/outputs with no driver |
| `diff <file2>` | Compare two DFG snapshots: show driver changes for every signal |
| `path <id1> <id2>` | Check if there is a forward dataflow path from node id1 to id2 |

`<name_or_id>` accepts either a signal name string or a numeric node id.

## Workflow for debugging a wrong signal value

1. `driver <signal>` — see what drives it directly
2. `cone <signal>` — see the full combinational dependency tree
3. `mux <signal>` — if the driver is a MUX, inspect the selector and branches
4. `diff <earlier.json>` — compare before/after a pass to see what changed
5. `op MUX` — list all MUX nodes to spot unexpected multiplexers
