# Phase 6: Switch `FlopInfo` and simulator to global domains

## Goal

Make flop scheduling semantic by using global `ClockId` / `ResetId` values
instead of translated local trigger strings.

## Scope (files/modules)

- `src/mateir/module.h`
- Global-domain resolution pass
- `src/frontends/systemverilog/passes/flop_resolve.cpp`
- `src/sim/simulator.h`
- `src/sim/simulator.cpp`
- `src/consumers/sim/simulator_consumer.cpp`

## Detailed steps

1. Add final `ClockId clock_domain` and `ResetDomains reset_domains` fields to
   `FlopInfo`.
2. Populate those fields from the global-domain resolver.
3. Change simulator construction so it receives `MateIR` or an explicit domain
   registry alongside the top module.
4. Change flop collection to group flops by `ClockId` and by every `ResetId` in
   `reset_domains`.
5. Convert top-level async source events into active clock/reset ID sets.
6. Apply reset effects by reset ID.
7. On clock events, skip flop updates when any of the flop's reset domains is
   currently active.
8. Keep old local trigger fields only for validation/debug during this phase.

## Acceptance criteria

- Simulator scheduling no longer depends on translated local clock/reset
  strings.
- Flops are grouped by `ClockId`; reset flops are grouped under every reset ID.
- Any active reset domain applies the flop's single `reset_value`.
- Existing externally driven clock/reset simulations remain behavior-equivalent.
- Regressions pass.

## Regression command

```bash
tests/regression.py
```

## Constraints (what must not change)

- Multiple reset domains mean multiple reset sources causing the same reset
  action.
- Reset/set chains or different forced reset values must be rejected, not
  approximated.
- Do not remove old local trigger fields yet.
- Do not migrate signal propagation or CDC checks yet.

## STOP condition

Stop once simulator scheduling uses global IDs, old local trigger fields are no
longer scheduler authority, and regression passes.
