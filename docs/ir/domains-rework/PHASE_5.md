# Phase 5: Resolve global domains in parallel

## Goal

Compute design-global clock/reset registries and future flop domain IDs while
old consumers still use old local fields.

## Scope (files/modules)

- New global-domain resolution pass in the SystemVerilog frontend
- `src/frontends/systemverilog/systemverilog_pipeline.cpp`
- Frontend-private facts from Phase 4
- `src/mateir/mateir.h`
- Debug/assertion helpers as needed

## Detailed steps

1. Add a global-domain resolution pass after `flop_resolve` and before any
   consumer migration.
2. Walk the elaborated hierarchy with instance paths.
3. Use frontend-private child input connection facts to resolve local
   clock/reset ports to parent sources.
4. Intern clock domains by `(HierSignalRef source, edge)`.
5. Intern reset domains by `(HierSignalRef source, active_edge)`.
6. Populate `MateIR::clocks` and `MateIR::resets`.
7. Compute each flop's future `ClockId` and `ResetDomains` into side tables or
   temporary parallel fields.
8. Validate every produced ID against its registry index.
9. Add assertions comparing old translated-string behavior to new IDs where
   practical.
10. Fail clearly on unsupported clock/reset connection expressions rather than
    guessing.

## Acceptance criteria

- `MateIR::clocks` and `MateIR::resets` are populated for existing supported
  designs.
- Repeated instances are resolved by instance path, not by module-local names.
- Same source plus same edge reuses an ID; same source plus different edge gets
  a different ID.
- Each flop has computed future domain IDs.
- Old consumers still pass regression using old fields.

## Regression command

```bash
tests/regression.py
```

## Constraints (what must not change)

- Existing consumers still use old fields.
- `display_name` is diagnostic only and must not be used as identity.
- `DFGNode*` must not be used as clock/reset domain identity.
- Do not solve generated/internal runtime behavior here; unsupported frontend
  cases must fail clearly.

## STOP condition

Stop once global registries and future flop IDs are computed and validated in
parallel, old behavior remains green, and no consumer has switched to the new
IDs yet.
