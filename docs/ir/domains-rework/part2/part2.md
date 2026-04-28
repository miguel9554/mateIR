## Overview

After flop resolve, we know, for each module, the _local_ signals acting as
clocks and resets for the _local_ flops.

The top-level domains YAML is read only for top-level input domain
classification. It tells us which top-level inputs are clocks, resets,
synchronous data, and asynchronous data. Optional `<module_name>.cdc.yaml`
sidecars are read separately for synchronizer flop annotations.

With the local flop clock/reset facts and the top-level domain seeds, we are
ready to run global domain resolve. That is, traversing the module hierarchy,
following clock/reset connections, and setting the global clock and reset
domains.

After this, we have defined:
* All the clock and reset domains
* All the flops and the relationship (ownership) between flops and domains

At this point we know:

* All the flops _global_ domains, which means, the domain of the flop Q output
* All the top level input domains

With these two facts, we are able to propagate the domains info through the DFG
to its outputs, which are:

* All the top level outputs
* All the flops D inputs

(for a precise definition of the propagation through DFG, see the section below)

In the middle we'll also propagate through the module signals/inputs/outputs, we
should record the final propagated domain of these public `Signal`s where useful.
This does not mean storing domain state as intrinsic `DFGNode` state.

Now we have reached the point where we know the domain of _all_ flop D inputs.
At this point we perform the CDC check:
* We check the domain of D, it _must_ be the same as the clock domain of the
  flop.
* We _only_ admit a different domain if the flop is defined as synchronizer.

Regarding sync definition, see section below explaining split w.r.t. current
YAML domain file.

## Summary of change in domains YAML use

YAML now _must_ be only used for
* Top level input domain classification

Internal modules should _no longer_ rely on the top-level domains YAML. YAML is
no longer needed for these modules. The only _optional_ companion file for an
internal module is the CDC sidecar that marks synchronizer flops (see below).

## Sync propagation through the DFG

Once global domains and flop ownership are resolved, sync-domain propagation is
an analysis over the DFG. The DFG itself remains the structural description of
combinational logic; propagated sync information should not be stored as
intrinsic state on each DFG node.

The only real domain sources are:

* Top-level inputs, whose external domain classification comes from YAML.
* Flop Q outputs, whose domain comes from the owning flop:
  `SyncSignal{flop.clock_domain, flop.reset_domains}`.

All other DFG node domains are derived by propagation. A combinational node does
not have an independently meaningful clock domain. Its output domain is computed
from its input domains using a simple rule:

* Clock domain:
  * If all non-constant inputs are `SyncSignal` in the same clock domain `D`,
    the output is `SyncSignal` in domain `D`.
  * Otherwise, the output is `AsyncSignal`.
* Reset domains:
  * If the output remains synchronous, its reset-domain set is the union of the
    input reset-domain sets.
  * If the output becomes asynchronous, reset-domain information is discarded.

Constants do not create a domain by themselves. They only participate when
combined with domain-carrying values.

This propagation produces domains for DFG outputs, especially:

* Flop D inputs.
* Top-level outputs.

For CDC validation, the important result is the propagated domain of each flop
D input. A flop clocked by domain `D` may only sample a D input that propagates
as `SyncSignal{clock_domain = D, ...}`.

```text
flop clock domain = D
flop D domain     = sync(D)  => valid
flop D domain     = sync(E)  => CDC violation, D != E
flop D domain     = async    => CDC violation
```

The only valid exception is an explicit synchronizer. A synchronizer is the
point where an async or cross-domain value is intentionally transformed into a
value synchronous to the target clock domain.

Therefore, CDC checking should be performed at flop D inputs, not at arbitrary
module port connections. Port connections are just dataflow edges: they should
propagate sync information through hierarchy. If an async value or a value from
another clock domain is connected into a child module, that connection is not
itself the error. It becomes an error only if the propagated value reaches a
flop whose clock domain does not match, without a synchronizer in between.

This is analogous to simulation values: values are not stored as permanent DFG
node state; they are computed by evaluating the graph from input/flop seeds.
Sync domains should be treated the same way for CDC analysis.

## Synchronizer annotations

The main top-level domains YAML should no longer describe synchronization with
per-signal `synchronized_into` attributes.

That attribute is attached to the wrong concept. A signal is not inherently
"synchronized into" a domain. A synchronizer is a receiving flop for which the
normal CDC rule is intentionally relaxed:

```text
normal flop:
  flop clock domain = D
  flop D domain     = sync(D)

synchronizer flop:
  flop clock domain = D
  flop D domain     = async or sync(E)
```

The synchronizer flop's Q output is still normal synchronous data in the flop's
clock domain:

```text
synchronizer flop Q domain = sync(D)
```

Therefore, synchronizer intent should be described by a small sidecar file that
travels with the RTL module that implements the synchronizer.

File name:

```text
<module_name>.cdc.yaml
```

Example:

```text
synchronizer_1bit.sv
synchronizer_1bit.cdc.yaml
```

The file does not need a `module_name` key. The module name is already part of
the file name.

Minimal format:

```yaml
synchronizer_flops:
  - sync_q1
```

Rules:

* Missing `<module_name>.cdc.yaml` means the module has no synchronizer flops.
* `synchronizer_flops` entries are exact local `FlopInfo::name` values after
  flop resolution.
* Unknown flop names are errors.
* Duplicate flop names are errors.
* The CDC exception applies only to the listed flop's D input.
* A synchronizer annotation only relaxes the CDC check for that flop's D input.
  It does not change propagation before the flop and it does not waive CDC for
  downstream flops.
* The listed flop's Q output is still seeded as
  `SyncSignal{flop.clock_domain, flop.reset_domains}`.

For a two-flop synchronizer, usually only the first flop is listed:

```yaml
synchronizer_flops:
  - meta_q
```

The second flop should normally sample `meta_q`, which is already synchronous to
the destination clock domain.

### Migration from `synchronized_into`

Current style:

```yaml
clock_domains:
  clk:
    polarity: posedge
    inputs_outputs:
      - async_i:
          synchronized_into: clk
```

Target style:

Top-level domains YAML classifies `async_i` only as async:

```yaml
async_domain:
  - async_i
```

The reusable synchronizer module carries its CDC sidecar:

```yaml
# synchronizer_1bit.cdc.yaml
synchronizer_flops:
  - meta_q
```

The RTL structure and normal propagation then determine the rest:

* `async_i` propagates as async into the synchronizer flop D input.
* `meta_q` is allowed to sample async because it is listed as a synchronizer
  flop.
* `meta_q.q` propagates as sync to the synchronizer clock domain.
* Downstream flops must again obey the normal CDC rule.

This keeps the top-level domains YAML focused on external unknowns, and keeps
reusable CDC intent next to the RTL module that implements it.
