# Phase 4: Add frontend-private domain facts

## Goal

Create frontend-private construction state for the future global domain resolver
while continuing to emit the old public IR fields.

## Scope (files/modules)

- `src/frontends/systemverilog/passes/io_domains_set.cpp`
- `src/frontends/systemverilog/passes/io_domains_set.h`
- `src/frontends/systemverilog/passes/flop_resolve.cpp`
- `src/frontends/systemverilog/passes/elaboration.cpp`
- New frontend-private structs/helpers if needed

## Detailed steps

1. Define frontend-private structures for YAML-derived local classifications.
2. Define frontend-private optional/unresolved sync construction state for
   signals before final `SyncType` assignment exists.
3. Preserve parsed timing/event-control facts needed to identify local clock and
   reset triggers.
4. Preserve child input connection facts needed to resolve child clock/reset
   ports to parent signals.
5. Keep diagnostic local names so future resolver errors can mention RTL names.
6. Refactor existing parsing/extraction code to populate these private facts
   while still writing all existing public fields.
7. Add narrow internal consistency checks where they do not change behavior.

## Acceptance criteria

- Private state can represent local classifications, trigger facts, connection
  facts, and unresolved sync construction state.
- Existing `Signal` and `FlopInfo` public fields are still populated exactly as
  before.
- Final public `MateIR` does not expose unresolved or frontend-local domain
  construction facts.
- Regressions pass; only equivalent diagnostic wording may change.

## Regression command

```bash
tests/regression.py
```

## Constraints (what must not change)

- Old fields remain authoritative for all consumers.
- No simulator or VCD migration.
- No global domain registry population yet.
- Do not add an unresolved/unknown case to public `SyncType`.

## STOP condition

Stop once private construction facts are available and old behavior still passes
regression. Do not add the global domain resolution pass in this phase.
