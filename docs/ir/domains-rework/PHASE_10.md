# Phase 10: Delete old domain fields and local trigger state

## Goal

Make final `MateIR` match the target clock/reset domain spec by removing
obsolete public fields.

## Scope (files/modules)

- `src/mateir/module.h`
- `src/mateir/module.cpp`
- Frontend passes that still write old fields
- Simulator, VCD, and static-analysis call sites
- `src/hierarchy.schema.json`
- `tools/hierarchy_inspect.py`
- Test tooling that reads hierarchy JSON clock/reset fields

## Detailed steps

1. Remove stored `SyncKind sync_kind`, `Signal* clock_domain`, and
   `std::optional<edge_t> clock_edge` from final `Signal`.
2. Keep `SyncKind` only as a derived helper if call sites still benefit from
   switch-friendly classification.
3. Remove `asyncTrigger_t clock` and `std::optional<asyncTrigger_t> reset` from
   final `FlopInfo`.
4. Keep parsed trigger facts only in frontend-private construction state.
5. Update print/debug/JSON serialization to derive display data from
   `SyncType`, `ClockDomain`, `ResetDomain`, and flop domain IDs.
6. Update hierarchy schema and inspection tools for the final representation.
7. Delete compatibility assertions that compare against removed old fields.

## Acceptance criteria

- Final public IR no longer stores local clock/reset semantic identity.
- Every signal has resolved `SyncType`.
- Every flop has valid `clock_domain`.
- Reset flops have valid `reset_domains` and one `reset_value`.
- Consumers no longer reconstruct domain identity through hierarchy names.
- Regressions pass.

## Regression command

```bash
tests/regression.py
```

## Constraints (what must not change)

- Do not remove useful diagnostics; derive them from final semantic data or
  frontend-private trigger facts.
- Do not add unresolved states to public `MateIR`.
- Do not use `display_name` as identity.
- Do not reintroduce local string or pointer domain identity.

## STOP condition

Stop once old public domain fields and local trigger state are removed, all
consumers build against final semantic IR, schema/tools are updated, and
regression passes.
