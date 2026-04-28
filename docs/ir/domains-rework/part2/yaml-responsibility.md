# YAML responsibility after domain inference

## Intent

The domains YAML should describe only design-boundary facts that are not
inferable from RTL.

The current per-module YAML files grew into a second source of truth for local
clock/reset facts. That overlaps with facts extracted from RTL by
`flop_resolve`, especially in internal modules.

Target principle:

```text
YAML owns external intent.
RTL owns internal clock/reset structure.
```

## YAML should specify

Top-level input domain classification:

- which top-level inputs are clocks
- clock input edge
- which top-level inputs are resets
- reset active polarity/edge
- which top-level data inputs are synchronous, and to which top-level clock
  domain
- which top-level data inputs are asynchronous

CDC/synchronizer intent:

- explicit `synchronized_into` annotations for boundary inputs or paths where
  the design intentionally synchronizes an async or cross-domain value into a
  clock domain

Optional, if needed by tooling:

- top-level output domain expectations for testbench/API presentation, but only
  if they cannot be derived robustly from propagated `SyncType`

## YAML should not specify

Internal module clock/reset declarations:

- internal module clock ports
- internal module reset ports
- internal module clock edges
- internal module reset polarities
- internal module sync data ports
- internal module output domains

Those facts should be inferred from RTL and hierarchy.

Example of YAML that should eventually disappear for internal modules:

```yaml
module_name: child

resets:
  rst_n:
    polarity: negative

clock_domains:
  clk:
    polarity: posedge
    inputs_outputs:
      - d
      - q
```

The RTL already says this if the module contains:

```systemverilog
always @(posedge clk or negedge rst_n) begin
  if (!rst_n) q <= 1'b0;
  else        q <= d;
end
```

## Inferred internal facts

`flop_resolve` should extract local facts such as:

- flop `q` is clocked by local signal `clk`
- the clock edge is `posedge`
- flop `q` is reset by local signal `rst_n`
- reset active edge is `negedge`
- reset value is `0`

Hierarchy elaboration should provide connection facts such as:

```text
u_child.clk   <- parent clk
u_child.rst_n <- parent rst_n
u_child.d     <- parent data
```

Global domain resolution should then trace clock/reset connections from the
top-level YAML seeds through the hierarchy and assign final `ClockId` /
`ResetId` values to flops and clock/reset signals.

## Pipeline direction

Current broad shape:

```text
io_domains_set
  reads YAML for every module

flop_resolve
  extracts local flop clock/reset usage

global_domain_resolve
  combines YAML facts, flop facts, and hierarchy connections
```

Target broad shape:

```text
top_yaml_domains
  seed only top-level external domains and synchronizer intent

flop_resolve
  extract local clock/reset trigger facts from every module

global_domain_resolve
  resolve internal clock/reset signal roles and global IDs through hierarchy

sync_type_propagation
  derive signal `SyncType` from top seeds, flop Q seeds, and DFG propagation
```

## Design consequence

Removing internal YAML also removes duplicated truth.

There should be no need to compare "YAML says this internal port is sync to
clock X" against "RTL says this flop uses clock Y". The RTL-derived local facts
and top-level YAML seeds are enough to construct the final semantic IR.

