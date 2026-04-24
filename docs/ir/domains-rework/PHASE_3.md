# Phase 3: Add domain datatypes, unused

## Goal

Introduce the final domain datatypes and `MateIR` registries without changing
pipeline behavior.

## Scope (files/modules)

- `src/mateir/module.h` or a new MateIR domain header
- `src/mateir/mateir.h`
- Build files only if a new source/header requires them

## Detailed steps

1. Add typed wrappers `ClockId` and `ResetId`, each carrying a `uint32_t value`
   and supporting comparison.
2. Add `ResetDomains` as a sorted unique vector of `ResetId`.
3. Add `InstancePath`, `SignalNamespace`, and `HierSignalRef`.
4. Add `ClockDomain` with `id`, `display_name`, `edge`, and `source`.
5. Add `ResetDomain` with `id`, `display_name`, `active_edge`, and `source`.
6. Add `std::vector<ClockDomain> clocks` and
   `std::vector<ResetDomain> resets` to `MateIR`.
7. Add only minimal helper declarations needed for compilation; leave behavior
   untouched.

## Acceptance criteria

- New types are available to later phases.
- `MateIR` owns empty clock/reset registries by default.
- Existing pipeline does not populate or consume the new registries.
- All existing tests continue to pass.

## Regression command

```bash
tests/regression.py
```

## Constraints (what must not change)

- Existing public IR fields remain authoritative.
- No consumer should depend on the new types yet.
- Do not replace `Signal::sync_kind`, `Signal::clock_domain`, or
  `FlopInfo::clock`.
- Do not introduce unresolved states into public `MateIR`.

## STOP condition

Stop once the new datatypes compile, registries exist on `MateIR`, registries
remain empty in current output, and regression passes.
