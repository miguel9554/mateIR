# Phase 8: Migrate propagation and CDC checks

## Goal

Make domain propagation and CDC validation use global IDs and final `SyncType`
semantics.

## Scope (files/modules)

- `src/frontends/systemverilog/passes/domains_propagate_and_check.cpp`
- Related pass headers
- `src/frontends/systemverilog/passes/io_domains_set.cpp`
- Synchronizer declaration handling
- Global-domain propagation helpers

## Detailed steps

1. Split or isolate current responsibilities in `domains_propagate_and_check`:
   validation, propagation, CDC checking, and connection-map trimming.
2. Replace pointer/string CDC comparisons with `ClockId` comparisons.
3. Propagate signal domains using final rules:
   same clock remains `SyncSignal`, multiple clocks become `AsyncSignal`, reset
   sets union only while the result remains synchronous.
4. Drop reset-domain information whenever a propagated result becomes
   `AsyncSignal`.
5. Use YAML only for top-level assumptions, synchronizer declarations, and
   consistency checks.
6. Ensure pure-combinational modules define no domains but receive propagated
   `SyncType` through their connected drivers.
7. Keep temporary assertions against old behavior where useful.

## Acceptance criteria

- CDC checks use `ClockId`.
- Propagated signals use final `SyncType` rules.
- Pure-combinational modules pass through `SyncType` without defining domains.
- Synchronizer declarations remain the explicit CDC escape hatch.
- Regressions pass or reveal intentional behavior differences that are handled
  within this phase.

## Regression command

```bash
tests/regression.py
```

## Constraints (what must not change)

- Do not depend on `Signal* clock_domain`.
- Do not preserve reset info on `AsyncSignal`.
- Do not remove `asyncPortConnections` yet if VCD still needs it.
- Do not delete old signal fields yet.

## STOP condition

Stop once propagation and CDC validation are fully driven by global IDs and
`SyncType`, old compatibility fields still exist, and regression passes.
