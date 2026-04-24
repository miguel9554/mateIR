# Clock/reset domain rework plan

## High-level goal

Rework clock/reset domain representation so final `MateIR` carries
design-global semantic domain IDs instead of local module signal names or
`Signal*` pointers.

The final IR must expose:

- `MateIR::clocks` and `MateIR::resets` as whole-design registries.
- Typed `ClockId` / `ResetId` references from signals and flops.
- `Signal::sync_type` as the authoritative synchronization state.
- `FlopInfo::clock_domain` and `FlopInfo::reset_domains` as authoritative
  sequential-domain metadata.
- No semantic dependency on local `asyncTrigger_t` names,
  `Signal* clock_domain`, per-signal `clock_edge`, or
  `Module::asyncPortConnections`.

Each phase must leave the tree coherent and pass:

```bash
tests/regression.py
```

## Phase 1: Mechanical `FlopInfo::type` cleanup

### Objective

Replace `FlopInfo::type` from `Signal` to `Type` without changing domain
behavior.

### Scope

- `src/mateir/module.h`
- `src/mateir/module.cpp`
- `src/frontends/systemverilog/passes/flop_resolve.cpp`
- Simulator, VCD, and static-analysis code that reads `flop.type.type` or
  `flop.type.binding`

### Risks

- Missing a `flop.type.type` access.
- Losing flop `.q` binding assumptions currently stored through embedded
  `Signal`.
- Accidentally changing hierarchy/debug JSON shape.

### Acceptance criteria

- `FlopInfo` stores value shape as `Type type`.
- Existing `flop.clock` / `flop.reset` remain unchanged.
- Existing `Signal::sync_kind`, `Signal::clock_domain`, and
  `Signal::clock_edge` remain unchanged.
- Debug/hierarchy JSON remains semantically equivalent.

### Regression command

```bash
tests/regression.py
```

### Invariants

- No new global-domain behavior.
- No simulator scheduling changes.
- No removal of `asyncTrigger_t`, `asyncPortConnections`, or old signal domain
  fields.

## Phase 2: Create `MateIR` earlier

### Objective

Make the frontend pipeline operate on `MateIR&` so later passes can own
design-global domain state.

### Scope

- `src/frontends/systemverilog/systemverilog_pipeline.h`
- `src/frontends/systemverilog/systemverilog_pipeline.cpp`
- `src/frontends/systemverilog/systemverilog_frontend.cpp`
- Call sites of `runMateIRPipeline`

### Risks

- Moving `Module` into `MateIR::top` too early and invalidating references.
- Debug output paths or module traversal accidentally changing.
- Consumers expecting `ir.top` only after the pipeline.

### Acceptance criteria

- `lowerSystemVerilogToMateIR` constructs `MateIR ir` before running the
  pipeline.
- `runMateIRPipeline` accepts `MateIR&`.
- Existing passes still operate on `ir.top`.
- No new domain fields or behavior are introduced.

### Regression command

```bash
tests/regression.py
```

### Invariants

- Pass order stays behavior-preserving.
- Source files and `frontend_module_count` are still populated.
- No change to serialized hierarchy or simulation behavior.

## Phase 3: Add domain datatypes, unused

### Objective

Introduce final domain datatypes and registries without migrating behavior.

### Scope

- `src/mateir/module.h` or a new MateIR domain header
- `src/mateir/mateir.h`
- Build files only if a new header/source is introduced

### Risks

- Weak ID typing causing accidental clock/reset mixups.
- Incomplete comparison/hash support for IDs or hierarchical refs.
- Header dependency cycles if domain types depend too heavily on `Module`.

### Acceptance criteria

- Add typed wrappers:
  - `ClockId { uint32_t value; }`
  - `ResetId { uint32_t value; }`
- Add `ResetDomains`, `ClockDomain`, `ResetDomain`, `InstancePath`,
  `SignalNamespace`, and `HierSignalRef`.
- Add `std::vector<ClockDomain> clocks` and `std::vector<ResetDomain> resets`
  to `MateIR`.
- Existing pipeline does not populate or consume the new fields yet.

### Regression command

```bash
tests/regression.py
```

### Invariants

- Empty domain registries are valid during this phase.
- Existing public IR fields remain authoritative.
- No consumer should depend on the new fields yet.

## Phase 4: Add frontend-private domain facts

### Objective

Create private construction scaffolding for domain resolution while continuing
to write old public IR fields.

### Scope

- `src/frontends/systemverilog/passes/io_domains_set.cpp`
- `src/frontends/systemverilog/passes/io_domains_set.h`
- `src/frontends/systemverilog/passes/flop_resolve.cpp`
- `src/frontends/systemverilog/passes/elaboration.cpp`
- New frontend-private structs/helpers if needed

### Risks

- Duplicating state and letting old/new facts diverge.
- Accidentally exposing partial resolution state in `MateIR`.
- Breaking existing YAML diagnostics.

### Acceptance criteria

- Frontend-private data can represent:
  - YAML local classifications.
  - Optional/unresolved sync construction state.
  - Parsed event controls / trigger facts.
  - Child input connection facts.
  - Diagnostic local names.
- Existing old fields are still populated exactly as before.
- No final `MateIR` public datatype contains unresolved sync state.

### Regression command

```bash
tests/regression.py
```

### Invariants

- `Signal::sync_kind`, `Signal::clock_domain`, `Signal::clock_edge`,
  `FlopInfo::clock`, and `FlopInfo::reset` remain authoritative for consumers.
- No simulator/VCD migration yet.
- No behavior change except possibly equivalent diagnostic wording.

## Phase 5: Resolve global domains in parallel

### Objective

Compute design-global clock/reset registries and future flop domain IDs while
old consumers still use old fields.

### Scope

- New global-domain resolution pass in the SystemVerilog frontend
- `src/frontends/systemverilog/systemverilog_pipeline.cpp`
- Frontend-private connection facts from elaboration
- `src/mateir/mateir.h`

### Risks

- Incorrect instance-path handling for repeated module instances.
- Incorrectly merging same local port names from different hierarchy contexts.
- Unsupported clock/reset connection expressions not diagnosed clearly.
- Old string-translation behavior and new ID behavior diverge silently.

### Acceptance criteria

- `MateIR::clocks` and `MateIR::resets` are populated.
- Domains are interned by:
  - Clock key: `(HierSignalRef source, edge)`
  - Reset key: `(HierSignalRef source, active_edge)`
- Each computed ID satisfies:
  - `id.value < registry.size()`
  - `registry[id.value].id == id`
- Each flop has computed future `ClockId` and `ResetDomains` stored in side
  tables or parallel temporary fields.
- Consistency assertions compare old translated-string behavior with new IDs
  where practical.

### Regression command

```bash
tests/regression.py
```

### Invariants

- Existing consumers still use old fields.
- `display_name` is diagnostic only, never identity.
- `DFGNode*` is not used as clock/reset domain identity.
- Unsupported generated/internal runtime behavior is not solved here;
  unsupported frontend cases must fail clearly.

## Phase 6: Switch `FlopInfo` and simulator to global domains

### Objective

Make flop scheduling semantic by using `ClockId` / `ResetId` instead of
translated local strings.

### Scope

- `src/mateir/module.h`
- `src/frontends/systemverilog/passes/flop_resolve.cpp`
- Global-domain resolution pass
- `src/sim/simulator.h`
- `src/sim/simulator.cpp`
- `src/consumers/sim/simulator_consumer.cpp`

### Risks

- Reset scheduling bugs when a flop has more than one reset domain.
- Simulator API needs `MateIR` or domain registries, not only `const Module&`.
- Active edge detection may accidentally keep depending on old trigger strings.

### Acceptance criteria

- `FlopInfo` has final domain fields:
  - `ClockId clock_domain`
  - `ResetDomains reset_domains`
- Simulator groups flops by `ClockId` and `ResetId`.
- A flop is grouped under every reset ID in `reset_domains`.
- Any active reset domain applies the flop's single `reset_value`.
- Clocked update skips a flop when any attached reset domain is currently
  active.
- Simulator no longer uses translated clock/reset strings for scheduling.

### Regression command

```bash
tests/regression.py
```

### Invariants

- Multiple reset domains mean multiple reset sources causing the same reset
  action.
- Reset/set chains or different forced reset values must be rejected, not
  approximated.
- Old local trigger fields may remain for validation/debug but are no longer
  scheduler authority.
- Existing top-level external clock/reset tests preserve behavior.

## Phase 7: Add `Signal::SyncType` in parallel

### Objective

Introduce final signal synchronization state without deleting old signal fields.

### Scope

- `src/mateir/module.h`
- `src/mateir/module.cpp`
- Global-domain resolution / propagation code
- Hierarchy JSON helpers if adding temporary debug output

### Risks

- `SyncType` and old `sync_kind` state diverge.
- Signals created before domain resolution need construction scaffolding.
- Variant use may make call sites verbose or inconsistent.

### Acceptance criteria

- `Signal` has `SyncType sync_type`.
- Final `MateIR` signals always have a concrete `SyncType`.
- Frontend may use optional/private unresolved state, but public IR does not.
- Add `syncKind(const Signal&)` helper for compatibility.
- Add assertions comparing old and new classification where practical.

### Regression command

```bash
tests/regression.py
```

### Invariants

- Old fields remain populated and available.
- No consumer migration is required in this phase.
- `AsyncSignal` carries no clock/reset/reset-domain data.
- If clock propagation becomes async, reset-domain influence is discarded.

## Phase 8: Migrate propagation and CDC checks

### Objective

Make domain propagation and CDC validation use global IDs and final sync
semantics.

### Scope

- `src/frontends/systemverilog/passes/domains_propagate_and_check.cpp`
- Related pass headers
- `src/frontends/systemverilog/passes/io_domains_set.cpp`
- Synchronizer declaration handling

### Risks

- Current pass mixes validation, propagation, CDC checks, and
  `asyncPortConnections` trimming.
- CDC behavior may reveal real old/new semantic differences.
- Pure-combinational modules need correct pass-through propagation.

### Acceptance criteria

- CDC checks compare `ClockId`, not `Signal*` or strings.
- Propagation rules are:
  - Same clock remains `SyncSignal`.
  - Multiple clocks become `AsyncSignal`.
  - Reset sets union only while result remains `SyncSignal`.
  - Async results discard reset-domain information.
- YAML is used for top-level assumptions, synchronizer declarations, and
  consistency checks.
- Pure-combinational modules define no domains but receive propagated
  `SyncType` from connected drivers.

### Regression command

```bash
tests/regression.py
```

### Invariants

- No dependence on `Signal* clock_domain`.
- No reset info preserved on `AsyncSignal`.
- Synchronizer declarations remain the explicit escape hatch for intended CDC.
- Existing accepted CDC tests remain accepted unless they relied on incorrect
  old behavior.

## Phase 9: Remove `asyncPortConnections` from final IR

### Objective

Delete the old hierarchy clock/reset translation mechanism after all consumers
use global IDs.

### Scope

- `src/mateir/module.h`
- `src/frontends/systemverilog/passes/elaboration.cpp`
- `src/frontends/systemverilog/passes/domains_propagate_and_check.cpp`
- `src/sim/vcd_writer.h`
- `src/sim/vcd_writer.cpp`
- `src/sim/simulator.cpp`

### Risks

- VCD child clock/reset mirrors may stop showing the expected source value.
- Hidden remaining simulator dependency on translated strings.
- Frontend still needs private port connection facts during construction.

### Acceptance criteria

- `Module::asyncPortConnections` is removed from final `MateIR`.
- Any needed child input connection facts live only in frontend-private state.
- VCD resolves child clock/reset values through:
  - `Signal::sync_type`
  - `ClockId` / `ResetId`
  - `MateIR` domain source
- Simulator has no translated-string clock/reset path.

### Regression command

```bash
tests/regression.py
```

### Invariants

- Removing final IR storage must not remove frontend construction evidence
  before resolution.
- Final IR still contains enough semantic data for simulator and VCD.
- Externally driven top-level clock/reset behavior stays unchanged.
- Internal/generated runtime behavior remains future work unless explicitly
  supported in implementation.

## Phase 10: Delete old domain fields and local trigger state

### Objective

Make final `MateIR` match the target spec and remove obsolete public state.

### Scope

- `src/mateir/module.h`
- `src/mateir/module.cpp`
- Frontend passes that still write old fields
- Simulator/VCD/static-analysis call sites
- `src/hierarchy.schema.json`
- `tools/hierarchy_inspect.py`
- Any test tooling that reads hierarchy JSON clock/reset fields

### Risks

- Tooling and JSON schemas may still expect old field names.
- Debug output may lose useful local trigger names if not preserved privately.
- Some old compatibility helper may hide a remaining semantic dependency.

### Acceptance criteria

- Remove from final `Signal`:
  - stored `SyncKind sync_kind`
  - `Signal* clock_domain`
  - `std::optional<edge_t> clock_edge`
- Remove from final `FlopInfo`:
  - `asyncTrigger_t clock`
  - `std::optional<asyncTrigger_t> reset`
- Keep `SyncKind` only as a derived helper if still useful.
- Keep parsed trigger facts only in frontend-private code.
- Hierarchy JSON/schema/tools expose final domain IDs or derived display
  strings from final semantic state.

### Regression command

```bash
tests/regression.py
```

### Invariants

- No public final IR field should encode local clock/reset semantic identity.
- Every signal has resolved `SyncType`.
- Every flop has valid `clock_domain`; reset flops have valid `reset_domains`
  and one `reset_value`.
- No consumer should need to reconstruct clock/reset identity through hierarchy
  names.

## Cross-phase test scenarios

Add focused tests as the relevant phase introduces behavior:

- Same child module instantiated twice with local `clk_i` connected to different
  top-level clocks.
- Two child-local clock names connected to the same top-level clock and edge
  resolve to one `ClockId`.
- Same source used with opposite clock edges resolves to distinct `ClockId`s.
- Same reset source used with opposite active edges resolves to distinct
  `ResetId`s.
- Synchronous combinational merge with same clock preserves `SyncSignal`.
- Multi-clock combinational merge becomes `AsyncSignal` and drops reset-domain
  info.
- Flop with two equivalent reset sources maps to two `ResetId`s and one
  `reset_value`.
- Flop with reset/set or different reset values is rejected.
- Pure-combinational module ports receive propagated `SyncType` but define no
  domains.

## Assumptions

- This plan is stored at `docs/ir/domains-rework/PLAN.md`.
- `tests/regression.py` is the required checkpoint after every phase.
- Internal/generated clock/reset runtime behavior is type-supported by
  `HierSignalRef` but not implemented unless a later phase explicitly adds
  runtime support.
- The implementation should prefer frontend-private construction state over
  adding unresolved states to public `MateIR`.
