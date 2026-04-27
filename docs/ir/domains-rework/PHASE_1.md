# Phase 1: Mechanical `FlopInfo::type` cleanup

## Goal

Replace `FlopInfo::type` from an embedded `Signal` with a plain `Type` while
preserving all current clock/reset behavior.

## Scope (files/modules)

- `src/mateir/module.h`
- `src/mateir/module.cpp`
- `src/frontends/systemverilog/passes/flop_resolve.cpp`
- Simulator, VCD, and analysis call sites that access `flop.type.type` or
  `flop.type.binding`

## Detailed steps

1. Change `FlopInfo` so its value-shape field is `Type type`.
2. Replace all `flop.type.type` reads and writes with `flop.type`.
3. Replace all uses of `flop.type.binding` with the existing authoritative
   `FlopInfo::binding` leaves.
4. Remove writes to duplicated or obsolete embedded-signal fields:
   `flop.type.name`, `flop.type.binding`, `flop.type.clock_domain`, and
   `flop.type.clock_edge`.
5. Update print/debug/JSON helpers to read the new field shape without changing
   the serialized meaning.
6. Build and fix mechanical compile errors only; do not introduce new domain
   semantics.

## Acceptance criteria

- `FlopInfo` stores its value shape as `Type`.
- Existing local `flop.clock` and `flop.reset` fields remain present and
  populated.
- Existing signal-domain fields remain present and populated.
- Debug output and hierarchy JSON are semantically equivalent to the previous
  output.
- The tree builds cleanly and the regression suite passes.

## Regression command

```bash
tests/regression.py
```

## Constraints (what must not change)

- No global clock/reset domain IDs are introduced.
- No simulator scheduling behavior changes.
- Do not remove `asyncTrigger_t`, `Signal::sync_kind`, `Signal::clock_domain`,
  `Signal::clock_edge`, or `Module::asyncPortConnections`.
- Do not change YAML domain parsing or CDC validation behavior.

## STOP condition

Stop at the first green regression run after the mechanical `FlopInfo::type`
conversion. Do not start moving `MateIR` ownership or adding domain datatypes in
this phase.
