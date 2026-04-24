# Clock/reset domain rework implementation notes

This document records implementation-level decisions for the datatype direction
described in `spec.md`.

It intentionally does not yet decide the detailed redesign of `io_domains_set`
or the exact frontend pass sequence for resolving global domains. Those topics
remain open.

## MateIR owns global domains

Global clock/reset domains are design-level objects, so they should live on
`MateIR`, not on individual `Module` objects.

Target shape:

```cpp
struct MateIR {
    Module top;
    std::vector<ClockDomain> clocks;
    std::vector<ResetDomain> resets;
    std::vector<std::string> source_files;
    size_t frontend_module_count = 0;
};
```

This implies changing the SystemVerilog frontend pipeline to create `MateIR`
earlier.

Current architecture:

```text
resolveModules(...) -> Module topModule
runMateIRPipeline(topModule, ...)
MateIR ir;
ir.top = std::move(topModule);
return ir;
```

Target architecture:

```text
MateIR ir;
ir.top = resolveModules(...);
runMateIRPipeline(ir, ...);
return ir;
```

Passes that need global clock/reset identity should operate on `MateIR&`, or on
a frontend-private context that eventually writes complete domain data into
`MateIR`.

## Final IR must be fully resolved

Final `MateIR` should not expose partially resolved or frontend-local clock/reset
facts.

In final IR:

- `MateIR::clocks` and `MateIR::resets` contain the unique global domains.
- `Signal::sync_type` directly references global clock/reset IDs when relevant.
- `FlopInfo` directly references its global clock ID and zero-or-more global
  reset IDs.
- Local Verilog trigger strings should not remain as semantic flop state.
- Local pointer links such as `Signal* clock_domain` should not remain.
- `asyncPortConnections` should not remain.

The frontend may still use temporary intermediate data structures while building
this final state.

## Frontend-private intermediate state is allowed

The SystemVerilog frontend will likely need extra working state that is not part
of MateIR.

Examples of acceptable frontend-private data:

- YAML-derived local classifications before global domain IDs are assigned.
- Local parsed event controls from `always_ff` / timing controls.
- Temporary maps from local trigger names to inferred clock/reset roles.
- Temporary instantiation input-connection facts.
- Temporary diagnostics data preserving the local source names that caused an
  error.

These structures should live inside the SystemVerilog frontend/pipeline
implementation, not in the public MateIR datatypes.

This distinction is important:

```text
Frontend-private state
  Evidence needed to construct the IR.

MateIR
  Fully resolved semantic IR consumed by simulator, VCD, analysis, and future
  consumers.
```

## Port connection facts are construction scaffolding

The final IR should not need a generic `inputPortConnections` or
`asyncPortConnections` map if domain resolution has already completed.

However, during global domain resolution, the frontend still needs to know how a
child input clock/reset port is connected to the parent.

Example:

```systemverilog
child u_child (
  .clk_i(core_clk),
  .rst_ni(top_rst_n)
);
```

To assign:

```cpp
u_child.inputs["clk_i"].sync_type = ClockSignal{core_clk_id};
u_child.inputs["rst_ni"].sync_type = ResetSignal{top_reset_id};
```

the frontend must first know:

```text
u_child.clk_i  <- parent core_clk
u_child.rst_ni <- parent top_rst_n
```

That connection information is an implementation input to domain resolution. It
is not itself a final IR concept.

Once global IDs are assigned to the child signals, the port connection facts can
be discarded.

## Removing asyncPortConnections

`Module::asyncPortConnections` should be removed from final MateIR.

Current behavior:

- It is populated during elaboration for simple identifier input connections.
- It is later trimmed to clock/reset ports.
- The simulator and VCD writer use it to translate child-local clock/reset names
  back to top-level signal names.

This is too low-level and too tied to the current SystemVerilog lowering.

Target behavior:

- Each clock/reset `Signal` has a `SyncType` variant that points to its global
  clock/reset domain.
- Each `FlopInfo` points to its global clock domain and reset-domain set.
- `ClockDomain::source` and `ResetDomain::source` identify the domain source.

Consumers should use those semantic facts instead of reconstructing domain
identity through a connection map.

## VCD impact

Today, VCD uses `asyncPortConnections` to mirror top-level async values into
child module clock/reset scopes.

In the new model, VCD should resolve this through domain IDs:

```text
child input clk_i
  -> SyncType::ClockSignal{ClockId 0}
  -> MateIR.clocks[0].source
  -> runtime value for that source
```

For externally driven top-level clocks/resets, the domain source will be a
top-level input signal, so this is straightforward.

Internally generated clocks/resets will require additional runtime handling in
the future, but that should not be solved by keeping `asyncPortConnections` in
the final IR.

## Simulator impact

The simulator should not need to recursively translate local flop trigger names
through hierarchy.

Current shape:

```cpp
std::map<std::string, std::vector<CollectedFlop>> flops_by_clock;
std::map<std::string, std::vector<CollectedFlop>> flops_by_reset;
```

Target shape:

```cpp
std::map<ClockId, std::vector<CollectedFlop>> flops_by_clock;
std::map<ResetId, std::vector<CollectedFlop>> flops_by_reset;
```

The simulator can collect flops from the hierarchy and group them directly by
`flop.clock_domain` and every ID in `flop.reset_domains`.

The main loop remains conceptually similar for externally driven clocks/resets:

- Read async events for top-level clock/reset input sources.
- Detect active edges.
- Convert active source signals to active `ClockId` / `ResetId` sets.
- Apply reset effects and clocked flop updates by domain ID.

Reset handling must account for reset-domain sets:

- A flop is grouped under every reset domain in `flop.reset_domains`.
- A reset event applies the flop's reset value when any of its reset domains is
  active.
- A clock event should skip updating a flop when any of its reset domains is
  currently active.

The current frontend may initially only produce reset-domain sets of size zero
or one, but the final IR and simulator should allow larger sets.

The non-trivial API change is that the simulator will need access to the global
domain registry, so it should consume `MateIR` or a domain registry alongside the
top module, rather than only `const Module&`.

## Open topics

Still to decide:

- How exactly `io_domains_set` should be split or replaced.
- The exact pass sequence for local YAML parsing, flop trigger extraction,
  global domain resolution, and CDC validation.
- The frontend-private representation for temporary port connection facts.
- How strict the frontend should be for unsupported clock/reset connection
  expressions during global domain resolution.
