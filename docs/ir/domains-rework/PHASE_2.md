# Phase 2: Create `MateIR` earlier

## Goal

Make the SystemVerilog frontend pipeline operate on `MateIR&` so later passes
can populate design-global state.

## Scope (files/modules)

- `src/frontends/systemverilog/systemverilog_pipeline.h`
- `src/frontends/systemverilog/systemverilog_pipeline.cpp`
- `src/frontends/systemverilog/systemverilog_frontend.cpp`
- Any direct call site of `runMateIRPipeline`

## Detailed steps

1. Change `lowerSystemVerilogToMateIR` to construct `MateIR ir` before running
   the pass pipeline.
2. Resolve the top module into `ir.top`.
3. Change `runMateIRPipeline` to accept `MateIR&`.
4. Inside the pipeline, keep existing pass logic operating on `ir.top`.
5. Preserve source file and frontend module count population on the returned
   `MateIR`.
6. Verify debug output still uses the top module name and same pass ordering.

## Acceptance criteria

- `runMateIRPipeline` accepts `MateIR&`.
- Existing passes still run in the same order on the same top module.
- `MateIR::source_files` and `MateIR::frontend_module_count` are still
  populated.
- No new domain behavior or new domain datatypes are required in this phase.
- The tree builds cleanly and the regression suite passes.

## Regression command

```bash
tests/regression.py
```

## Constraints (what must not change)

- Do not change pass order.
- Do not change serialized hierarchy or simulation behavior.
- Do not add or populate `MateIR::clocks` or `MateIR::resets`.
- Do not change ownership semantics outside the frontend pipeline.

## STOP condition

Stop once the pipeline receives `MateIR&`, all existing passes still operate
through `ir.top`, and regression passes. Do not add domain datatypes in this
phase.
