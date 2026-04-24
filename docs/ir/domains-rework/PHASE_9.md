# Phase 9: Remove `asyncPortConnections` from final IR

## Goal

Delete the old hierarchy clock/reset translation mechanism after simulator and
VCD can use global domain IDs.

## Scope (files/modules)

- `src/mateir/module.h`
- `src/frontends/systemverilog/passes/elaboration.cpp`
- `src/frontends/systemverilog/passes/domains_propagate_and_check.cpp`
- `src/sim/vcd_writer.h`
- `src/sim/vcd_writer.cpp`
- `src/sim/simulator.cpp`

## Detailed steps

1. Move any remaining child input connection facts needed for construction into
   frontend-private state.
2. Remove `Module::asyncPortConnections` from final `MateIR`.
3. Remove trimming logic that exists only to prepare `asyncPortConnections` for
   simulator/VCD consumers.
4. Update VCD setup so child clock/reset values resolve through
   `Signal::sync_type`, domain ID, and `MateIR` domain source.
5. Verify simulator has no remaining translated-string clock/reset path.
6. Remove stale comments and debug assumptions that describe
   `asyncPortConnections` as final IR.

## Acceptance criteria

- `Module::asyncPortConnections` is absent from final `MateIR`.
- Frontend still has enough private connection evidence before resolution.
- VCD mirrors externally driven child clock/reset values through domain IDs.
- Simulator and VCD no longer reconstruct clock/reset identity from local names.
- Regressions pass.

## Regression command

```bash
tests/regression.py
```

## Constraints (what must not change)

- Do not remove frontend-private construction facts before resolution.
- Externally driven top-level clock/reset behavior must stay unchanged.
- Internal/generated runtime behavior remains future work unless explicitly
  implemented.
- Do not delete old signal fields or local trigger fields yet unless all
  consumers are already migrated.

## STOP condition

Stop once `asyncPortConnections` is gone from final IR, simulator/VCD use domain
IDs, and regression passes.
