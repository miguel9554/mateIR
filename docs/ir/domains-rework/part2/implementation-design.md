# Part 2 implementation design

## Goal

Move from per-module YAML domain declarations to a model where:

* Top-level domains YAML describes only external top-level input domains.
* RTL/flop extraction describes internal clock/reset structure.
* Optional `<module_name>.cdc.yaml` sidecars mark synchronizer flops.
* Sync-domain propagation is a DFG analysis from top input and flop Q seeds.
* CDC checking is performed at flop D inputs.

The main architectural change is removing internal module YAML as a source of
truth.

## Current pipeline

Current relevant order:

```text
io_domains_set
flop_resolve
global_domain_resolve
dce
domains_propagate_and_check
```

Current responsibilities:

* `io_domains_set`
  * Reads a domains YAML file for every module.
  * Classifies ports as clock/reset/sync/async.
  * Carries per-module sync input/output domain declarations.
  * Carries `synchronized_into`.
* `flop_resolve`
  * Extracts local flop triggers and reset values from RTL/DFG.
  * Records local clock/reset trigger facts.
  * Still relies on per-module YAML facts for validation.
* `global_domain_resolve`
  * Uses per-module YAML port facts, child connection facts, and flop trigger
    facts to create global `ClockId` / `ResetId` registries.
  * Assigns final domains to flops.
* `domains_propagate_and_check`
  * Seeds sync types from per-module YAML port facts and flop Qs.
  * Propagates through the DFG with a temporary node map.
  * Assigns public `Signal::sync_type`.
  * Performs CDC checks and some cross-module connection checks.

## Target pipeline

Target relevant order:

```text
flop_resolve
load_top_io_domains
load_cdc_annotations
global_domain_resolve
dce
sync_domain_propagate
cdc_check
```

`dce` can remain between global domain resolution and sync propagation as long
as it preserves all nodes needed to analyze live flop D inputs and top outputs.

## Target stage responsibilities

### `flop_resolve`

Owns local RTL-derived sequential facts.

It should extract, for every module occurrence:

* local flop name
* local clock signal name
* clock edge
* optional local reset signal name
* reset active edge
* reset value
* D and Q leaf bindings

It should not depend on per-module domains YAML to know whether a local signal
is a clock or reset. A local signal is a clock/reset because it appears in a
supported sequential event control and reset structure.

It may still validate structural constraints:

* unsupported trigger count
* unsupported reset expression
* reset value not static
* different reset sources forcing different values, until the IR supports that

Output:

```text
ModuleDomainFacts::flop_domains
FlopInfo reset_value / binding / local names needed privately
```

Longer-term, these facts can be renamed away from `ModuleDomainFacts` into a
frontend-private `SequentialFacts` structure.

### `load_top_io_domains`

Replaces the top-level-input portion of `io_domains_set`.

Reads only the top-level domains YAML. It should not require or read internal
module domains YAML files.

Owns external facts that RTL cannot infer:

* top-level clock input names and edges
* top-level reset input names and active edges
* top-level synchronous data input names and their top-level clock domain
* top-level asynchronous data input names

It should reject:

* YAML for non-top modules
* unclassified top-level inputs, unless the selected policy permits defaults
* references to missing top-level ports
* duplicate classification of the same top-level port
* `synchronized_into`, which is removed from the main domains YAML

Output should be a small frontend-private structure, for example:

```cpp
struct TopInputDomainFacts {
    std::map<std::string, TopClockInputFact> clocks;
    std::map<std::string, TopResetInputFact> resets;
    std::map<std::string, TopSyncInputFact> sync_inputs;
    std::set<std::string> async_inputs;
};
```

### `load_cdc_annotations`

Reads optional per-module sidecars:

```text
<module_name>.cdc.yaml
```

Minimal format:

```yaml
synchronizer_flops:
  - meta_q
```

Owns synchronizer intent:

* a listed flop may sample async or foreign-domain D input
* the listed flop Q still belongs to the flop clock domain

It should validate after `flop_resolve`, because by then local `FlopInfo::name`
values are known:

* unknown flop names are errors
* duplicate entries are errors
* invalid YAML shape is an error

Missing sidecar means no synchronizer flops.

Output:

```cpp
struct ModuleCdcFacts {
    std::set<std::string> synchronizer_flops;
};
```

or an occurrence-aware equivalent if module specialization/generate expansion
requires it.

### `global_domain_resolve`

Keeps its name, but changes inputs.

Target inputs:

* top-level clock/reset seeds from `load_top_io_domains`
* local flop trigger facts from `flop_resolve`
* hierarchy child input connection facts from elaboration

It should no longer depend on per-module YAML port facts for internal modules.

Responsibilities:

* create/intern global clock domains by `(HierSignalRef source, edge)`
* create/intern global reset domains by `(HierSignalRef source, active_edge)`
* trace local clock/reset signals through hierarchy to top-level seeds
* assign `FlopInfo::clock_domain`
* assign `FlopInfo::reset_domains`
* validate all assigned IDs

Important inference change:

```text
current:
  child port is a clock because child YAML says it is a clock

target:
  child port is a clock because a local flop trigger uses that signal as a clock
```

Unsupported cases should remain explicit errors:

* local flop clock/reset is not traceable to a supported parent connection
* clock/reset connection expression is not supported
* local trigger resolves to a top-level signal not declared as matching
  clock/reset seed
* reset source forces unsupported reset behavior

### `sync_domain_propagate`

Split out of `domains_propagate_and_check`.

Runs after global domain IDs are assigned.

Inputs:

* top-level input `SyncType` seeds from top YAML facts
* flop Q `SyncType` seeds from `FlopInfo`
* DFG structure

It should use a temporary analysis map:

```cpp
std::map<const DFGNode*, SyncType> node_sync;
```

DFG nodes should not store propagated `SyncType` as intrinsic state.

Propagation rule:

* all non-constant inputs are `SyncSignal` with same `ClockId` => output is
  `SyncSignal` in that clock domain
* reset-domain sets are unioned while output remains synchronous
* async input, clock/reset-as-data input, or mixed sync clocks => output is
  `AsyncSignal`
* constants do not create domains by themselves

Persistent outputs:

* public `Signal::sync_type` for top-level inputs, outputs, and internal signals
  where useful
* a pass-local or returned analysis result for flop D input sync types

Suggested result object:

```cpp
struct SyncDomainAnalysis {
    std::map<const DFGNode*, SyncType> node_sync;
    std::map<const FlopInfo*, SyncType> flop_d_sync;
};
```

The full node map can remain private if only signal assignment and CDC need it.

### `cdc_check`

Split out of `domains_propagate_and_check`.

Runs after sync propagation.

For every flop:

```text
flop clock domain = D
flop D domain     = sync(D) => valid
flop D domain     = sync(E) => invalid unless synchronizer
flop D domain     = async   => invalid unless synchronizer
```

Synchronizer exception:

* If the flop is listed in `<module_name>.cdc.yaml`, the D mismatch is allowed.
* The exception applies only to that flop D input.
* Downstream flops are checked normally.

This pass should not reject arbitrary module port connections. Port connections
are dataflow edges; propagation determines what reaches each flop D.

## Refactor phases

The refactor should not be split into one implementation phase per conceptual
responsibility. A smaller four-phase migration is easier to reason about:

```text
Phase 1: Split current propagation/check responsibilities.
Phase 2: Add CDC sidecars and move synchronizer intent to flops.
Phase 3: Restrict main domains YAML to top-level inputs.
Phase 4: Infer internal domains from RTL and propagate by dataflow.
```

### Phase 1: Split current propagation/check responsibilities

Keep behavior mostly equivalent, but make the code boundaries match the target
model.

Work:

* Split the implementation of `domains_propagate_and_check` into propagation
  and CDC-check helpers.
* Introduce a `SyncDomainAnalysis` result object or equivalent local structure.
* Keep the existing per-module YAML behavior for now.
* Keep current tests passing.

Expected result:

* There is still one pipeline stage if desired, but internally there are clear
  functions for:
  * seeding and propagating `SyncType`
  * assigning public `Signal::sync_type`
  * checking CDC
* No semantic migration has happened yet.

### Phase 2: Add CDC sidecars

Move synchronizer intent from per-signal `synchronized_into` to per-flop
annotations in `<module_name>.cdc.yaml`.

Work:

* Add a parser/loader for `<module_name>.cdc.yaml`.
* Add frontend-private CDC facts containing `synchronizer_flops`.
* Validate sidecar entries against local `FlopInfo::name` after `flop_resolve`.
* Teach CDC check to allow D-domain mismatch only for listed synchronizer flops.
* Migrate existing `synchronized_into` tests/modules to sidecars.
* Reject `synchronized_into` in the main domains YAML.

Expected result:

* Synchronizer intent is attached to receiving flops.
* Main domains YAML no longer carries synchronizer information.
* Flop Q propagation remains unchanged: synchronizer flop Q is still sync to
  the flop clock domain.

### Phase 3: Restrict main domains YAML to top-level inputs

Refactor `io_domains_set` into a top-level-only domain loader.

Work:

* Require only the top module to have a domains YAML.
* Stop recursively loading YAML for internal modules.
* Keep only top-level input classification:
  * clocks
  * resets
  * synchronous data inputs
  * asynchronous data inputs
* Update test scaffolding and test YAML files.

Expected result:

* Internal modules no longer need `*.domains.yaml`.
* Top-level YAML owns only external unknowns.
* The code may still carry compatibility structures internally, but they are no
  longer populated from per-module YAML.

### Phase 4: Infer internal domains from RTL and propagate by dataflow

This is the main semantic migration.

Work:

* Update `global_domain_resolve` so internal clock/reset roles come from
  `flop_resolve` facts, not module YAML port facts.
* For each module occurrence:
  * collect local clock trigger signal names from flop facts
  * collect local reset trigger signal names from flop facts
  * resolve those local signals through hierarchy child-input connection facts
  * intern global domains from the resolved top-level source and edge
  * assign resulting IDs to flops
* Remove the assumption that child input `SyncType` comes from child YAML.
* Seed sync propagation only from:
  * top-level input domains
  * flop Q domains
* Propagate through the DFG/hierarchy dataflow.
* Check CDC only at flop D inputs.
* Remove or reduce cross-module connection checks once flop-D CDC is
  authoritative.

Expected result:

* Internal clock/reset structure is inferred from RTL.
* Public `Signal::sync_type` is derived from propagation.
* Flop D CDC is checked with the rule:

```text
only sync(flop.clock_domain) may drive a normal flop D
```

* The only exception is a listed synchronizer flop.

See `PLAN.md` in this directory for a file/pass-oriented checklist.

## Open implementation questions

* Should top-level output domain expectations remain in YAML, or should outputs
  always be derived from propagation?
* Should constant-only flop D inputs be accepted in any clock domain by context,
  or should propagation assign constants to the consuming flop domain during CDC
  checking?
* Are synchronizer flop names exact post-`flop_resolve` names only, or do we
  need pattern support for generated arrays?
* Should CDC sidecars be located by source file directory, include path search,
  explicit CLI arguments, or all of these?
