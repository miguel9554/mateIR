# Top-Level Domain Inference Plan

This plan adds a no-YAML top-level domain inference mode. Each phase must end
with a clean regression run:

```bash
python3 tests/regression.py
```

The with-YAML path must remain supported throughout the work. The no-YAML path
should infer the same frontend domain facts that `loadTopIODomains()` currently
builds, then rejoin the existing global-domain resolution and sync/CDC checks.

## Phase 1: Infer Top Clocks And Resets

### Goal

Allow compilation without a top-level `.domains.yaml` for designs where all
top-level clock/reset ports are discoverable from flop trigger facts.

### Scope

- Add an explicit frontend option for top-level domain inference.
- Stop requiring `--domains` when inference mode is enabled.
- Reuse the existing flop trigger facts recorded by `flop_resolve`.
- In infer mode, populate `FrontendDomainFacts::top_inputs->clocks` and
  `top_inputs->resets` from inferred top-level clock/reset demands.
- Keep the with-YAML path behavior-preserving.

### Expected Code Areas

- `src/main.cpp`
- `src/frontends/frontend.h`
- `src/frontends/systemverilog/systemverilog_pipeline.cpp`
- `src/frontends/systemverilog/passes/global_domain_resolve.cpp`
- Possibly `src/frontends/systemverilog/passes/io_domains_set.{h,cpp}` if
  shared top-input fact helpers are introduced.

### Key Rules

- A top-level input used as any flop clock is a clock input.
- A top-level input used as any extracted flop reset is a reset input.
- Clock identity includes edge.
- Reset identity includes active polarity.
- Conflicting roles or conflicting reset polarity are hard errors.
- In with-YAML mode, existing validation remains strict.

### Tests

- Add at least one existing-style design that compiles in infer mode with no
  domains YAML.
- Add a conflict/error test if practical: one top input inferred as both clock
  and reset, or as a reset with conflicting polarities.
- Confirm existing YAML tests remain clean.

### Acceptance Criteria

- Inference mode can reach `global_domain_resolve` without a YAML file.
- Top-level inferred clocks/resets receive valid `ClockId` / `ResetId` values.
- Existing regression suite is clean.

## Phase 2: Infer Top Data Inputs From Flop D Cones

### Goal

Infer synchronous and asynchronous top-level data input classification from RTL
dataflow instead of YAML `inputs_outputs` and `async_domain`.

### Scope

- Add a pass or helper that runs after global clock/reset IDs are resolved.
- Walk backward from each flop `.d` leaf.
- Collect top-level data input leaves in each D cone.
- For each collected top input, add a constraint to the sampling flop's
  `clock_domain`.
- Populate `FrontendDomainFacts::top_inputs->sync_inputs` and
  `top_inputs->async_inputs`.

### Expected Code Areas

- New pass, likely
  `src/frontends/systemverilog/passes/top_io_domains_infer.{h,cpp}`.
- `src/frontends/systemverilog/systemverilog_pipeline.cpp`
- `src/frontends/systemverilog/domain_facts.{h,cpp}` if report structures or
  helper facts are added.
- `src/mateir/module.{h,cpp}` helpers for identifying top input leaves, if
  existing helpers are insufficient.

### Key Rules

- Ignore top inputs already classified as clock or reset.
- A top input found in D cones for exactly one clock domain is synchronous to
  that clock domain.
- A top input found in D cones for multiple clock domains is classified async
  and reported.
- A top input with no sequential D-cone evidence is classified async and
  reported.
- Constants and flop `.q` leaves do not create top-input constraints.
- The pass should walk from flop D roots backward; it should not run one full
  graph traversal per input unless needed for simplicity.

### Tests

- Single-clock design: data inputs infer into that clock domain.
- Multi-clock design: shared data input becomes async or reports multidomain.
- Unused or output-only top input becomes async and is reported.
- Existing with-YAML behavior remains unchanged.

### Acceptance Criteria

- In infer mode, top-level non-clock/reset inputs are fully classified before
  normal `domains_propagate_and_check`.
- Normal sync propagation/check can run using inferred facts.
- Existing regression suite is clean.

## Phase 3: Synchronizer-Aware Inference

### Goal

Make inference respect explicit CDC synchronizer annotations, and define the
initial no-sidecar behavior.

### Scope

- Load CDC sidecars before data-input inference if they exist.
- When a top input reaches a CDC-marked synchronizer flop D cone, classify that
  top input async instead of adding a sync constraint to the synchronizer flop's
  clock.
- Keep no-sidecar behavior conservative and report suspected synchronizer cases
  instead of silently accepting arbitrary CDC.

### Expected Code Areas

- `src/frontends/systemverilog/passes/cdc_annotations.{h,cpp}`
- New top I/O inference pass from phase 2.
- `src/frontends/systemverilog/passes/cdc_check.cpp`
- `src/frontends/systemverilog/systemverilog_pipeline.cpp`

### Key Rules

- CDC sidecar present and flop listed as synchronizer:
  - top inputs in that flop's D cone become async evidence;
  - do not add a sync constraint from that flop clock.
- CDC sidecar absent:
  - do not require an annotation file just to run inference;
  - report CDC-like mismatches found by normal checks as candidates for an
    explicit `.cdc.yaml`;
  - do not broadly bless every cross-domain sample unless the policy is made
    explicit in this phase's detailed plan.

### Tests

- A marked synchronizer makes its source top input async in inferred domains.
- A sidecar-marked synchronizer still passes normal CDC checking.
- A no-sidecar cross-domain sample produces the chosen diagnostic/report.
- Existing CDC-sidecar tests remain clean.

### Acceptance Criteria

- Inferred domains do not incorrectly mark explicit synchronizer sources as
  synchronous to the destination clock.
- CDC sidecar behavior is compatible with the existing with-YAML flow.
- Existing regression suite is clean.

## Phase 4: CLI, YAML Emission, Reports, And Hardening

### Goal

Make the feature usable from the command line and stable enough for repeated
regression use.

### Scope

- Add final CLI surface for no-YAML inference.
- Add optional inferred YAML emission.
- Add concise reports for async-by-policy inputs:
  - unused inputs;
  - output-only inputs;
  - multidomain inputs;
  - synchronizer-related inputs.
- Add docs and regression tests for the user-visible behavior.

### Expected Code Areas

- `src/main.cpp`
- `src/frontends/frontend.h`
- `src/frontends/systemverilog/systemverilog_pipeline.cpp`
- YAML emission helper, possibly adjacent to `io_domains_set`.
- `README.md` or dedicated docs if user-facing CLI docs are updated.

### Key Rules

- Inferred YAML should match `src/domains.schema.json`.
- Domain names should be deterministic.
- Preserve top input declaration order where practical for readable output.
- Emission should be optional; inference should be usable without writing a
  file.
- Reports should be deterministic and testable.

### Tests

- CLI no-YAML compile test.
- YAML emission golden test or schema/roundtrip test.
- Deterministic output order test if golden output is used.
- Full regression suite.

### Acceptance Criteria

- A user can compile with inferred top-level domains and no `.domains.yaml`.
- A user can ask for an inferred YAML file and feed it back into the normal
  YAML path.
- Reports explain every top input classified async by lack of stronger evidence.
- Existing regression suite is clean.

## Out Of Scope For These Four Phases

- Inferring output domains as YAML schema data.
- Supporting generated/internal clocks beyond the current global-domain model.
- Removing the with-YAML path.
- Changing the public `SyncType` lattice for inference-only uncertainty.
- Auto-emitting `.cdc.yaml` unless explicitly added by a later plan.
