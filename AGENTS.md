# Project Instructions

## Project

Synchronous Verilog/SystemVerilog compiler that lowers RTL into `MateIR`.

Top-level sidecar metadata lives in:
- `.domains.yaml` for top input clock/reset/sync/async classification
- `.cdc.yaml` for intentional synchronizer flops

## Core invariant: MateIR

`MateIR` contains:
- the top `Module` hierarchy
- global clock/reset registries
- source file metadata

`Module` contains:
- inputs, outputs, and internal module nodes
- flops
- DFG
- submodule hierarchy

The DFG is not the source of truth. When iterating ports, signals, or flops,
iterate `Module` data structures and bindings, not DFG maps.

## Working rules

- Prioritize correctness and simplicity.
- Never fail silently; throw immediately on bad states.
- Do only the requested work.
- If testing reveals unrelated failures, stop and ask before fixing them.
- Before designating any file or structure as the source of truth, ask first.

## Canonical environment

The repo provides a canonical Docker environment via `Dockerfile` and
`scripts/docker-run.sh`. Prefer running build, test, and compiler commands
through `scripts/docker-run.sh` unless the current environment is already known
to match the repo's toolchain. The wrapper expects the `mate-dev:latest` image,
which can be built with `scripts/docker-build.sh`.

## Slang rules

Allowed:
- `external/slang/include/slang/syntax/SyntaxTree.h`
- `external/slang/include/slang/syntax/SyntaxVisitor.h`
- `external/slang/scripts/syntax.txt` via targeted grep only

Do not read unless explicitly asked:
- generated slang syntax headers
- slang `.cpp`, tests, or docs
- other slang files

## Canonical entry points

Build:
- `make dev`
- `make sanitized`
- `make debug`

Per-test:
- `make -C tests/<name>/work/validate validate`
- `make -C tests/<name>/work/static analyze`
- `make -C tests/<name>/work/custom-sim simulate`

Regression uses:
- `python tests/regression.py`
- `tests/check_dfg_api_surface.py`
- `tests/check_module_node_api_surface.py`

Regression has two mutually exclusive `--mode` values for PASS tests
(`--mode` default is `validate`):
- `validate` (default): runs `make validate` in `tests/<name>/work/validate`,
  the custom vector-based simulator compared against a VCD reference. Judged
  by the `make` process exit code.
- `verilator-dpi`: runs `make simulate DPI=1` in `tests/<name>/work/verilator`,
  which builds the generated native DPI model and compares it against
  Verilator/RTL. Only applies to tests that have a `work/verilator` dir;
  others are skipped in this mode. The DPI checker's mismatch signal is a
  `$fatal(1)` inside a `final` block, which does not set a non-zero process
  exit code, so pass/fail is judged by scanning stdout/stderr for the
  `PASS: 100% match` sentinel (or a `DPI and RTL mismatched` line), not the
  `make` return code.

`make regression` was removed; run `python tests/regression.py` directly
(add `--mode verilator-dpi` for the DPI path).

## Important files

- `src/main.cpp`: CLI and top-level modes / flags
- `src/frontends/frontend.h`: frontend options
- `src/frontends/systemverilog/systemverilog_pipeline.cpp`: pass order and debug artifact emission
- `src/frontends/systemverilog/domain_facts.h`: domain / CDC fact model
- `src/mateir/module.h`: module / flop / binding invariants
- `src/domains.schema.json`
- `src/cdc.schema.json`

## Debug artifacts

Compiler runs emit under `debug_output/<top>/`, commonly inside per-test work
directories.

Important artifacts:
- per-pass DFG `.json` and `.dot`
- `hierarchy.json`
- flop snapshots around domain / flop passes

Prefer repo tools over ad hoc scripts:
- `tools/dfg_inspect.py`
- `tools/hierarchy_inspect.py`
- `tools/vcd_inspect.py`

Shared workflow skills live under `skills/`. 
