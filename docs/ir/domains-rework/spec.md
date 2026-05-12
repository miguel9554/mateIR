# Top-Level Domain Inference Spec

## Purpose

The compiler currently requires a top-level `.domains.yaml` file to classify
top-level inputs as clocks, resets, synchronous data, or asynchronous data.
This file carries information that is largely complementary to the RTL. The RTL
already exposes:

- which inputs act as clocks or resets for flops;
- which top-level data inputs reach flop D cones;
- which clock domain samples each flop D cone.

Top-level domain inference uses those facts to build the same
`FrontendDomainFacts::top_inputs` structure that YAML loading builds today.
After inference, the normal global-domain resolution, sync propagation, and CDC
checks should run without knowing whether the facts came from YAML or RTL.

## Modes

The compiler has two top-level-domain modes:

- **YAML mode**: existing behavior. A top-level `.domains.yaml` is loaded and
  validated. Inferred clock/reset demands must agree with the YAML facts.
- **Infer mode**: no top-level `.domains.yaml` is required. The frontend
  infers top-level clock/reset/data classifications from the elaborated RTL.

CDC sidecars are independent from the top-level domains YAML. In infer mode,
CDC sidecars may still be loaded and should influence data-input inference.

## Pipeline Placement

Inference relies on facts that exist only after flop resolution. The intended
shape is:

```text
elaboration
dfg_inline
constant_fold
type_propagation
condition_normalization
constant_fold
flop_resolve
load_top_io_domains OR infer top clock/reset facts
cdc_annotations
global_domain_resolve
infer top data input facts, in infer mode
dce
domains_propagate_and_check
```

The exact pass boundaries can change during implementation, but these
dependencies must hold:

- flop trigger facts exist before clock/reset inference;
- global `ClockId` / `ResetId` values exist before data-input inference stores
  final clock-domain names or IDs;
- CDC annotations are available before synchronizer-aware data-input inference;
- normal sync propagation/check runs after inferred top inputs are populated.

## Clock And Reset Inference

A top-level input is inferred as a clock when it is the local clock signal for
any flop occurrence, directly or through a supported hierarchy input alias.

A top-level input is inferred as a reset when it is the extracted reset signal
for any flop occurrence, directly or through a supported hierarchy input alias.

Clock identity includes edge:

```text
(top input, posedge) != (top input, negedge)
```

Reset identity includes active polarity:

```text
(top input, active high) != (top input, active low)
```

Conflicts are hard errors:

- one top input inferred as both clock and reset;
- one top reset input inferred with multiple active polarities;
- unsupported hierarchy expression needed to trace an inferred clock/reset to a
  top-level input.

Clock domain names in emitted YAML should be deterministic. Prefer the top port
name when there is one inferred domain for that port. If disambiguation is
needed, append an edge suffix.

Reset names in emitted YAML should also be deterministic. Prefer the top port
name when there is one inferred reset domain for that port.

## Data Input Inference

Data input inference classifies top-level inputs that are not clocks or resets.
It does not require an uncertainty domain in the public `SyncType` lattice.

For every flop occurrence:

1. Take the resolved functional D leaf or leaves after `flop_resolve`.
2. Walk backward through the DFG cone.
3. Collect top-level input leaves reached by that cone.
4. Ignore top inputs classified as clocks or resets.
5. For each remaining top input, add evidence from the sampling flop.

For ordinary non-synchronizer flops, the evidence is:

```text
top input P reaches flop F.d => P must be valid in F.clock_domain
```

After all flops are processed:

- zero clock-domain constraints: classify the input async and report why;
- exactly one clock-domain constraint: classify the input sync to that domain;
- more than one clock-domain constraint: classify the input async and report a
  multidomain reason.

This rule intentionally uses the sampling flop clock, not the propagated domain
of the D expression. That keeps inference independent from an optimistic or
unknown sync type.

## Synchronizer-Aware Inference

CDC sidecars can mark local flops that intentionally sample asynchronous or
foreign-domain data.

When a CDC sidecar exists and a flop is marked as a synchronizer:

- top data inputs found in that synchronizer flop's D cone are async evidence;
- the synchronizer flop clock must not create a sync constraint for those
  inputs;
- normal CDC checking should still treat the marked flop as a synchronizer.

When no CDC sidecar exists, infer mode should not require one. The initial
policy is conservative:

- run inference without explicit synchronizer exemptions;
- let normal propagation/check identify mismatches;
- report likely synchronizer candidates clearly;
- avoid silently accepting all cross-domain samples as intentional.

A later feature may emit candidate `.cdc.yaml` entries, but that is not part of
the base spec.

## Async Classification Reasons

Inference should retain deterministic reasons for async classification so users
can review generated YAML.

Recommended reason categories:

- `unused`: the top input is not found in any relevant flop D cone;
- `output_only`: the top input only influences top-level combinational outputs;
- `multidomain`: the top input reaches flops in multiple clock domains;
- `synchronizer`: the top input reaches CDC-marked synchronizer flop D cones;
- `explicit_async`: reserved for future override files, if added.

The public schema still represents all of these as `async_domain`; the reasons
are diagnostic/report metadata, not YAML schema data.

## YAML Emission

Infer mode may optionally emit a `.domains.yaml` file matching
`src/domains.schema.json`.

The emitted YAML must include:

- `module_name`;
- `resets`;
- `clock_domains`;
- `async_domain`, if non-empty.

Each inferred clock domain should contain `polarity` and `inputs_outputs`.
Synchronous data inputs inferred into that domain should appear in
`inputs_outputs`.

Each inferred reset should contain `polarity`. Use `signal_name` only when the
reset domain name differs from the input port name.

Emission should be deterministic:

- stable domain ordering;
- stable input ordering;
- no wildcard generation in the first implementation;
- no output-port classification unless the YAML schema grows to support it.

The emitted YAML should be valid input to the existing with-YAML path.

## Compatibility Requirements

- YAML mode remains strict and behavior-preserving.
- Infer mode produces the same internal facts as YAML mode before normal sync
  propagation/check.
- The public `SyncType` model does not gain an inference-only
  `UnknownOptimistic` state.
- Existing global clock/reset ID invariants remain valid.
- Existing CDC sidecar semantics remain valid.

## Known Limitations

- Inputs that do not reach any flop D cone cannot be proven synchronous and are
  classified async by policy.
- Inputs that reach multiple unrelated clock domains are classified async by
  policy.
- Without CDC sidecars, automatic synchronizer recognition is intentionally
  conservative.
- Generated clocks and internally produced reset domains are limited by the
  existing global-domain resolver capabilities.
- Top-level output domains are not represented in the current YAML schema and
  are out of scope.
