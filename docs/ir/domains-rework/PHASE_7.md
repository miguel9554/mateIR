# Phase 7: Add `Signal::SyncType` in parallel

## Goal

Introduce final signal synchronization state while keeping old signal domain
fields for compatibility.

## Scope (files/modules)

- `src/mateir/module.h`
- `src/mateir/module.cpp`
- Global-domain resolution and propagation code
- Hierarchy JSON/debug helpers if temporary visibility is useful

## Detailed steps

1. Add `SyncSignal`, `ClockSignal`, `ResetSignal`, `AsyncSignal`, and
   `SyncType`.
2. Add `SyncType sync_type` to `Signal`.
3. Populate `sync_type` from global-domain resolution and propagation.
4. Keep optional/unresolved sync construction state frontend-private only.
5. Add `syncKind(const Signal&)` as a derived compatibility helper.
6. Add practical assertions comparing old `sync_kind` / pointer state to new
   `sync_type`.
7. Keep old fields present and populated for consumers.

## Acceptance criteria

- Every final `Signal` has a concrete resolved `SyncType`.
- Public `MateIR` has no unresolved sync variant.
- `syncKind(const Signal&)` derives the old classification from `sync_type`.
- Assertions catch obvious old/new classification mismatches.
- Regressions pass with old consumers still available.

## Regression command

```bash
tests/regression.py
```

## Constraints (what must not change)

- Old signal fields remain populated.
- No required consumer migration in this phase.
- `AsyncSignal` carries no clock, reset, or reset-domain payload.
- If propagation becomes async, reset-domain influence is discarded.

## STOP condition

Stop once `SyncType` is present and populated in parallel, all signals are
resolved in final IR, old fields still work, and regression passes.
