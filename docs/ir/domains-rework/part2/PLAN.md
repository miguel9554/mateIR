# Part 2 refactor plan

This plan turns the design in `part2.md` and `implementation-design.md` into
four implementation phases.

## Phase 1: Split propagation and CDC checking

### Goal

Clarify responsibilities before changing YAML semantics.

Current `domains_propagate_and_check` does several jobs:

* validates local trigger facts against YAML port facts
* seeds node sync types
* propagates sync types through the DFG
* assigns public `Signal::sync_type`
* performs CDC checks
* performs cross-module connection checks

This phase keeps behavior mostly equivalent but splits the implementation into
clearer units.

### Expected result

At the end of this phase:

* Propagation and CDC checking are separate helpers or pass modules.
* A `SyncDomainAnalysis`-like object exists, even if it is initially private to
  the same pipeline stage.
* Regression behavior should be unchanged except for deliberate bug fixes.
* Existing per-module domains YAML is still supported.
* Existing `synchronized_into` is still supported.

### Files/passes involved

Primary:

* `src/frontends/systemverilog/passes/domains_propagate_and_check.cpp`
* `src/frontends/systemverilog/passes/domains_propagate_and_check.h`

Possible new files:

* `src/frontends/systemverilog/passes/sync_domain_propagate.cpp`
* `src/frontends/systemverilog/passes/sync_domain_propagate.h`
* `src/frontends/systemverilog/passes/cdc_check.cpp`
* `src/frontends/systemverilog/passes/cdc_check.h`

Pipeline:

* `src/frontends/systemverilog/systemverilog_pipeline.cpp`

### Notes

This phase is mostly mechanical. Avoid changing the source of truth for domains
here.

Cross-module connection checks should be isolated so they can later be removed
or reduced once flop-D CDC is authoritative.

## Phase 2: Move synchronizer intent to CDC sidecars

### Goal

Replace per-signal `synchronized_into` with per-flop synchronizer annotations.

New sidecar format:

```text
<module_name>.cdc.yaml
```

Minimal content:

```yaml
synchronizer_flops:
  - meta_q
```

### Expected result

At the end of this phase:

* Optional `<module_name>.cdc.yaml` files are loaded.
* Listed synchronizer flops may sample async or foreign-domain D inputs.
* The exception applies only to the listed flop D input.
* The listed flop Q is still seeded as sync to the flop clock domain.
* Existing designs that used `synchronized_into` are migrated to CDC sidecars.
* The main domains YAML rejects or ignores `synchronized_into` according to the
  chosen migration strictness. Preferred final behavior is to reject it.

### Files/passes involved

Primary:

* `src/frontends/systemverilog/domain_facts.h`
* `src/frontends/systemverilog/domain_facts.cpp`
* `src/frontends/systemverilog/systemverilog_pipeline.cpp`
* `src/frontends/systemverilog/passes/domains_propagate_and_check.cpp`
  or the new `cdc_check` files from Phase 1

YAML parsing:

* New CDC sidecar loader, likely under:
  * `src/frontends/systemverilog/passes/`
  * or `src/frontends/systemverilog/`

Current YAML parser to update:

* `src/frontends/systemverilog/passes/io_domains_set.cpp`
* `src/domains.schema.json`

Tests/tooling:

* existing tests using `synchronized_into`
* `tests/regression_tests.txt`
* `tools/new_test.py`
* `tools/gen_tb.py`, if it needs to discover CDC sidecars

### Tests to add or migrate

* async reaches normal flop D => fail
* sync(D1) reaches normal flop(D2) => fail
* async reaches synchronizer flop D => pass
* synchronizer Q reaches normal flop in same domain => pass
* unknown flop in `<module>.cdc.yaml` => fail
* duplicate flop in `<module>.cdc.yaml` => fail

## Phase 3: Restrict main domains YAML to top-level inputs

### Goal

Remove internal module domains YAML from the pipeline.

The main domains YAML should classify only top-level inputs:

* top-level clocks
* top-level resets
* top-level synchronous data inputs
* top-level asynchronous data inputs

It should not classify internal module ports.

### Expected result

At the end of this phase:

* Only the top module requires a domains YAML file.
* Internal modules no longer need `*.domains.yaml`.
* `io_domains_set` is either renamed or reduced to top-level-only behavior.
* Top-level YAML no longer carries `synchronized_into`.
* Tests no longer provide internal domains YAML files.

### Files/passes involved

Primary:

* `src/frontends/systemverilog/passes/io_domains_set.cpp`
* `src/frontends/systemverilog/passes/io_domains_set.h`
* `src/frontends/systemverilog/systemverilog_pipeline.cpp`
* `src/frontends/systemverilog/domain_facts.h`
* `src/frontends/systemverilog/domain_facts.cpp`

Possible rename:

```text
io_domains_set -> load_top_io_domains
```

Command-line/domain file handling:

* `src/frontends/systemverilog/systemverilog_pipeline.cpp`
* `src/frontends/systemverilog/systemverilog_frontend.cpp`
* `src/main.cpp`, if CLI assumptions change

Schemas/tooling:

* `src/domains.schema.json`
* `README.md`
* `tools/new_test.py`
* `tools/gen_tb.py`
* `tools/hierarchy_inspect.py`, if output assumptions change

Tests:

* remove internal `*.domains.yaml` files
* keep one top-level domains YAML per top test
* keep `<module>.cdc.yaml` where needed

### Notes

This phase narrows YAML ownership but may still leave some internal compatibility
facts in memory. Those should be removed in Phase 4.

## Phase 4: Infer internal domains from RTL and check flop D inputs

### Goal

Complete the semantic migration.

Internal clock/reset roles come from `flop_resolve`, not YAML. Sync domains are
propagated from top inputs and flop Q outputs through the DFG. CDC is checked at
flop D inputs.

### Expected result

At the end of this phase:

* `global_domain_resolve` resolves global clock/reset IDs from:
  * top-level clock/reset seeds
  * local flop trigger facts
  * hierarchy connection facts
* `FlopInfo::clock_domain` and `FlopInfo::reset_domains` are fully resolved.
* `Signal::sync_type` is derived from propagation, not child module YAML.
* Flop D CDC checking uses propagated D domains.
* Cross-module connection checks are removed or limited to structural sanity.
* Obsolete per-module port domain facts are removed or made construction-only
  for top-level inputs.

### Files/passes involved

Primary:

* `src/frontends/systemverilog/passes/global_domain_resolve.cpp`
* `src/frontends/systemverilog/passes/global_domain_resolve.h`
* `src/frontends/systemverilog/passes/flop_resolve.cpp`
* `src/frontends/systemverilog/passes/flop_resolve.h`
* `src/frontends/systemverilog/domain_facts.h`
* `src/frontends/systemverilog/domain_facts.cpp`
* `src/frontends/systemverilog/passes/domains_propagate_and_check.cpp`
  or the new propagation/CDC files from Phase 1

DFG/signal helpers:

* `src/mateir/module.h`
* `src/mateir/module.cpp`
* `src/mateir/domains.h`

Consumers to verify:

* `src/sim/simulator.cpp`
* `src/sim/vcd_writer.cpp`
* `tools/hierarchy_inspect.py`
* `src/hierarchy.schema.json`

### Tests to add or verify

* internal module with no domains YAML, flops infer clock/reset from RTL
* child clock/reset connected to top-level clock/reset resolves correctly
* repeated child instances share global domains when connected to same top
  source and edge
* same source with opposite edges creates distinct clock domains
* async top input through normal logic to normal flop fails
* sync(D1) through normal logic to flop(D2) fails
* async top input into synchronizer flop passes
* synchronizer Q into normal same-domain flop passes
* top output domain is derived from propagation

### Notes

This phase should remove the current reset-domain loss at child input
boundaries, because child input sync type is no longer assigned from child YAML.
It is propagated from the actual connected parent value.

## Final state

After all phases:

* The main domains YAML owns only top-level external input domains.
* Internal module clock/reset facts are inferred from RTL.
* Synchronizer intent is represented by `<module_name>.cdc.yaml`.
* DFG nodes do not store persistent sync domains.
* Sync propagation is a temporary analysis from top input and flop Q seeds.
* CDC is checked at flop D inputs.
* Normal flops only accept `sync(flop.clock_domain)` on D.
* Synchronizer flops are the only exception.

