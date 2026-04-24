# Clock/reset domain rework phases

This rework is large enough that it should not be implemented as one change.
Each phase should leave the tree in a coherent state and should pass:

```bash
tests/regression.py
```

The guiding principle is:

```text
Compute new data in parallel first.
Switch consumers second.
Delete old data last.
```

## Phase 1: Mechanical FlopInfo type cleanup

Goal: remove the obviously wrong `FlopInfo::type = Signal` coupling.

Changes:

- Change `FlopInfo::type` from `Signal` to `Type`.
- Replace `flop.type.type` with `flop.type`.
- Remove writes to:
  - `flop.type.name`
  - `flop.type.binding`
  - `flop.type.clock_domain`
  - `flop.type.clock_edge`
- Keep existing `flop.clock` / `flop.reset` local `asyncTrigger_t` fields.
- Keep existing `Signal::sync_kind`, `Signal* clock_domain`, and
  `Signal::clock_edge`.

Expected behavior:

- Behavior-preserving.
- No domain semantic changes yet.

Checkpoint:

```bash
tests/regression.py
```

## Phase 2: Create MateIR earlier in the pipeline

Goal: make the pipeline capable of owning design-global state.

Changes:

- Change `lowerSystemVerilogToMateIR` to construct `MateIR ir` earlier.
- Move the resolved top module into `ir.top` before running the pipeline.
- Change `runMateIRPipeline(Module&)` to `runMateIRPipeline(MateIR&)`.
- Internally, existing passes can still operate on `ir.top`.
- Do not add new domain behavior yet.

Expected behavior:

- Behavior-preserving.
- This is plumbing for later global domain state.

Checkpoint:

```bash
tests/regression.py
```

## Phase 3: Add new domain datatypes, unused

Goal: introduce the final domain datatypes without migrating behavior.

Changes:

- Add:
  - `ClockId`
  - `ResetId`
  - `ResetDomains`
  - `ClockDomain`
  - `ResetDomain`
  - `InstancePath`
  - `SignalNamespace`
  - `HierSignalRef`
- Add to `MateIR`:

```cpp
std::vector<ClockDomain> clocks;
std::vector<ResetDomain> resets;
```

- Do not change existing passes yet.

Expected behavior:

- Behavior-preserving.

Checkpoint:

```bash
tests/regression.py
```

## Phase 4: Add frontend-private domain facts

Goal: make room for the new flow without exposing partial state in MateIR.

Changes:

- Add SystemVerilog frontend-private structures for:
  - YAML-derived local classifications.
  - Local parsed event controls from timing blocks.
  - Local clock/reset trigger facts from flop resolution.
  - Temporary instantiation input-connection facts.
  - Diagnostics data preserving local source names.
- Refactor current parsing/extraction code to populate these structures where
  useful.
- Continue writing old fields so downstream code still works:
  - `Signal::sync_kind`
  - `Signal::clock_domain`
  - `Signal::clock_edge`
  - `FlopInfo::clock`
  - `FlopInfo::reset`

Expected behavior:

- Intended to be behavior-preserving.
- Error messages may change slightly if parsing code is reorganized.

Checkpoint:

```bash
tests/regression.py
```

## Phase 5: Resolve global domains in parallel

Goal: compute the new global clock/reset domains while old consumers still use
old fields.

Changes:

- Add a new global-domain resolution pass after `flop_resolve`.
- Use local flop trigger facts as the source of truth.
- Walk hierarchy using frontend-private connection facts.
- Create:

```cpp
MateIR::clocks
MateIR::resets
```

- Compute each flop's future:

```cpp
ClockId clock_domain;
ResetDomains reset_domains;
```

- Store those new results either in temporary side tables or in parallel fields.
- Do not remove old `FlopInfo::clock` / `FlopInfo::reset`.
- Add consistency assertions comparing old translated-string behavior with new
  IDs where practical.

Expected behavior:

- Existing observable behavior should remain unchanged.
- The new pass may fail loudly on internal inconsistencies.

Checkpoint:

```bash
tests/regression.py
```

## Phase 6: Switch FlopInfo and simulator to global domains

Goal: make flops semantic and stop simulator grouping by translated strings.

Changes:

- Add final fields to `FlopInfo`:

```cpp
ClockId clock_domain;
ResetDomains reset_domains;
```

- Populate them from the global-domain resolution pass.
- Migrate simulator flop grouping from:

```cpp
std::map<std::string, std::vector<CollectedFlop>> flops_by_clock;
std::map<std::string, std::vector<CollectedFlop>> flops_by_reset;
```

to:

```cpp
std::map<ClockId, std::vector<CollectedFlop>> flops_by_clock;
std::map<ResetId, std::vector<CollectedFlop>> flops_by_reset;
```

- Group each flop under every reset ID in `flop.reset_domains`.
- Keep old local trigger fields temporarily if needed for validation/debug, but
  stop simulator consumers from using them.

Expected behavior:

- Simulator behavior should remain the same for existing tests.
- This is a major checkpoint because runtime scheduling now uses domain IDs.

Checkpoint:

```bash
tests/regression.py
```

## Phase 7: Add Signal SyncType in parallel

Goal: introduce final signal synchronization state without deleting old fields.

Changes:

- Add `SyncType sync_type` to `Signal`.
- Populate it from global-domain resolution and propagation.
- Keep old fields temporarily:
  - `Signal::sync_kind`
  - `Signal::clock_domain`
  - `Signal::clock_edge`
- Add helper functions:

```cpp
SyncKind syncKind(const Signal&);
```

- Add assertions where practical to compare old and new classification state.

Expected behavior:

- Behavior-preserving.
- Assertions should catch mismatches between old local-domain state and new
  global-domain state.

Checkpoint:

```bash
tests/regression.py
```

## Phase 8: Migrate domain propagation and CDC checks

Goal: make domain propagation and CDC validation use global IDs.

Changes:

- Replace pointer/string CDC checks with `ClockId` comparisons.
- Propagate signal clock/reset info using final rules:
  - same clock domain remains `SyncSignal`
  - multiple clock domains become `AsyncSignal`
  - reset-domain sets are unioned
- Use YAML only for:
  - top-level input assumptions
  - synchronizer declarations
  - consistency checks
- Stop depending on `Signal* clock_domain`.

Expected behavior:

- This is the core semantic migration.
- Some failures may reveal real behavior differences; keep this phase focused.

Checkpoint:

```bash
tests/regression.py
```

## Phase 9: Remove asyncPortConnections from final IR

Goal: delete the old hierarchy translation mechanism.

Changes:

- Remove `Module::asyncPortConnections`.
- Keep any needed port connection facts frontend-private only.
- Update VCD to resolve child clock/reset display values through:

```text
Signal::sync_type -> ClockId/ResetId -> MateIR domain source
```

- Ensure simulator no longer uses translated strings anywhere.

Expected behavior:

- Behavior should remain the same.
- This is a major checkpoint because simulator and VCD both used
  `asyncPortConnections`.

Checkpoint:

```bash
tests/regression.py
```

## Phase 10: Delete old fields and local trigger state from final IR

Goal: make final MateIR match the target spec.

Changes:

- Remove from `Signal`:

```cpp
SyncKind sync_kind;
Signal* clock_domain;
std::optional<edge_t> clock_edge;
```

- Keep `SyncKind` only as a derived helper enum if useful.
- Remove from final `FlopInfo`:

```cpp
asyncTrigger_t clock;
std::optional<asyncTrigger_t> reset;
```

- Keep `asyncTrigger_t` or equivalent parsed trigger facts only inside
  SystemVerilog frontend-private code if still needed.

Expected behavior:

- Final structural cleanup.
- Final MateIR should match `spec.md`.

Checkpoint:

```bash
tests/regression.py
```

## Summary order

Recommended order:

1. Mechanical `FlopInfo::type` cleanup.
2. Create `MateIR` earlier in the pipeline.
3. Add new domain datatypes, unused.
4. Add frontend-private domain facts.
5. Resolve global domains in parallel.
6. Switch `FlopInfo` and simulator to global domains.
7. Add `Signal::SyncType` in parallel.
8. Migrate propagation and CDC checks to global IDs.
9. Remove `asyncPortConnections`.
10. Delete old fields and local trigger state from final IR.
