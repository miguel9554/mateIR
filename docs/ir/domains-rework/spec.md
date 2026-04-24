# Clock/reset domain datatype rework

## Problem

The current IR represents clock/reset information mostly as local module facts.
For example, `FlopInfo` stores its clock as an `asyncTrigger_t`, which contains
a string name. That string is local to the module that owns the flop. If a child
module has an input port named `clk_i`, then flops inside that child store
`clk_i` as their clock, even if the parent connects that port to a top-level
clock named `core_clk`.

That means the semantic clock identity is not directly present in the IR. A
consumer must trace local names through hierarchy input connections to discover
which unique design-level clock actually drives a local signal or flop.

The target direction is:

- The whole design has a unique set of clock domains.
- The whole design has a unique set of reset domains.
- Local signals directly state their synchronization type and, when relevant,
  reference the corresponding global clock/reset domain.
- Flops directly reference their global clock domain and zero-or-more global
  reset domains.
- Local RTL trigger names can still exist as parser/debug information, but they
  should not be the semantic identity used by consumers.

This document is only about datatype design. It intentionally does not decide
which pipeline pass creates the global clock/reset domains or how the frontend
performs top-down domain resolution.

## Global domain IDs

Use stable typed IDs over raw C++ pointers as the primary references.

Pointers are convenient inside one process, but IDs are easier to serialize,
copy, move, inspect, and test. The API can still provide helper accessors that
feel pointer-like.

```cpp
struct ClockId {
    uint32_t value;
    auto operator<=>(const ClockId&) const = default;
};

struct ResetId {
    uint32_t value;
    auto operator<=>(const ResetId&) const = default;
};
```

Use distinct wrapper types instead of `using ClockId = uint32_t` /
`using ResetId = uint32_t`. Clock and reset IDs are used widely enough that
accidentally passing a reset ID where a clock ID is expected should be a compile
error.

IDs are local to one `MateIR` domain registry. In final IR, every ID reference
must satisfy:

```cpp
id.value < registry.size()
registry[id.value].id == id
```

That makes the vectors the owning registries and keeps lookup cheap while still
allowing IDs to be copied through the IR.

Clock IDs and reset IDs identify global domain objects. Clock membership is
singular: a synchronous value is synchronous to exactly one known clock domain.
If multiple clock domains meet, the result is asynchronous because it is not
synchronous to any single known clock.

Reset membership is not singular for synchronous values. A synchronous value can
be influenced by zero, one, or many reset domains while still being synchronous
to one clock domain. Represent reset membership as a sorted unique set:

```cpp
struct ResetDomains {
    std::vector<ResetId> ids; // sorted unique
};
```

An empty set means no reset-domain dependency is present in final IR.

Reset membership is not carried by `AsyncSignal`. If a value is asynchronous,
the IR does not try to preserve which reset sources may have influenced it. The
consumer must treat it as asynchronous and synchronize it before using it in a
clocked domain.

The unique domain objects should live at the whole-design IR level, for example
on `MateIR`, or temporarily on the top module if that is more convenient during
migration.

```cpp
struct ClockDomain {
    ClockId id;
    std::string display_name;
    edge_t edge;
    HierSignalRef source;
};

struct ResetDomain {
    ResetId id;
    std::string display_name;
    edge_t active_edge;
    HierSignalRef source;
};
```

`display_name` is for diagnostics and JSON output. It should not be the
authoritative identity of the domain.

The semantic key used when creating/interning domains is:

- Clock domain: `(source, edge)`
- Reset domain: `(source, active_edge)`

Therefore `posedge clk` and `negedge clk` on the same source signal are two
different clock domains, and active-high versus active-low use of the same reset
source are two different reset domains. Once a domain is interned, the
authoritative identity used by the rest of the IR is its ID.

`source` is the signal occurrence that originates the clock/reset domain. For a
simple external clock, that source is a top-level input port. In the future, for
internally generated clocks, the source may be an internal signal, a submodule
output, a flop output, or another hierarchical signal occurrence.

Edge/polarity belongs on the global domain object, not on every local signal
that refers to the domain.

The domain object does not record every local signal occurrence that aliases or
receives the domain. Those local references are artifacts of the hierarchy and
connection structure. The global object only records the unique domain identity
and its source. If a later consumer needs fanout information, such as which
flops or modules a domain feeds, that should be represented as a separate
derived index.

## Hierarchical signal references

Do not use a plain string like `"u_pll.clk_div2"` as the in-memory identity for
a clock/reset source. Dot-encoded strings are acceptable as derived diagnostic
text, but they are ambiguous as IR.

Use a structured reference to a signal occurrence in the elaborated hierarchy:

```cpp
struct InstancePath {
    std::vector<std::string> elems; // empty means top
};

enum class SignalNamespace {
    Input,
    Output,
    Internal,
    FlopQ,
    FlopD,
};

struct HierSignalRef {
    InstancePath instance_path;
    SignalNamespace ns;
    std::string name;
};
```

Examples:

```cpp
// Top-level input clk
HierSignalRef{
    .instance_path = {},
    .ns = SignalNamespace::Input,
    .name = "clk",
};

// Internal signal u_pll.clk_div2
HierSignalRef{
    .instance_path = InstancePath{{"u_pll"}},
    .ns = SignalNamespace::Internal,
    .name = "clk_div2",
};

// Flop output u_div.counter.q
HierSignalRef{
    .instance_path = InstancePath{{"u_div"}},
    .ns = SignalNamespace::FlopQ,
    .name = "counter",
};
```

The structured form avoids parsing ambiguity around dots, generated-block names,
escaped identifiers, and the difference between inputs, outputs, internal
signals, and flop state.

## DFG nodes are not domain identity

Clock and reset identity should not be represented by `DFGNode*`.

Clocks and resets are currently not modeled as normal DFG dataflow edges. Reset
logic is extracted into flop metadata, and clock/reset events are asynchronous
control metadata rather than ordinary combinational values in the DFG.

For that reason, `DFGNode*` should not participate in clock/reset identity. The
semantic IR contract should be the structured hierarchical reference. If some
implementation later needs to look up a DFG node for a non-clock/non-reset data
signal, that should be a derived lookup outside the domain identity datatype.

## Signal synchronization type

The current `Signal` stores clock/reset classification as loose fields:

```cpp
struct Signal : SignalBase {
    SyncKind sync_kind = SyncKind::Sync;
    Signal* clock_domain = nullptr;
    std::optional<edge_t> clock_edge;
    SignalBinding binding;
};
```

This should be replaced. In particular:

- Remove `Signal* clock_domain`. It is a local pointer to another `Signal`, not
  a global domain identity.
- Remove per-signal `clock_edge`. Clock edge and reset polarity live on
  `ClockDomain` / `ResetDomain`.
- Do not store `SyncKind` independently from the domain payload, because that
  allows invalid combinations.
- Do not use `TypeMetadata` for clock/reset domains. Domain membership is
  instance/signal metadata, not value type metadata.

Use a variant to encode the valid synchronization cases directly:

```cpp
struct SyncSignal {
    ClockId clock_domain;
    ResetDomains reset_domains;
};

struct ClockSignal {
    ClockId clock_domain;
};

struct ResetSignal {
    ResetId reset_domain;
};

struct AsyncSignal {};

using SyncType = std::variant<
    SyncSignal,
    ClockSignal,
    ResetSignal,
    AsyncSignal
>;

struct Signal : SignalBase {
    SyncType sync_type;
    SignalBinding binding;
};
```

The invariants are encoded by the variant:

- `SyncSignal`: synchronous data. It must have a clock domain and may have a
  zero-or-more reset domains.
- `ClockSignal`: clock signal. It carries/defines exactly one clock domain.
- `ResetSignal`: reset signal. It carries/defines exactly one reset domain.
- `AsyncSignal`: asynchronous data. It has no clock/reset domain and does not
  preserve reset-domain influence.

Final `MateIR` must contain a concrete resolved `SyncType` for every `Signal`.
There is intentionally no `UnresolvedSignal` / `UnknownSignal` case in this
public IR datatype. The frontend may use `std::optional<SyncType>`, side tables,
or other private construction state while resolving domains, but unresolved
state must not escape into final IR.

When signal domains are propagated through combinational logic, clock domains
and reset domains combine differently:

- Clock domain: keep the clock only when all contributors have the same
  `ClockId`; if multiple clock domains meet, the result is `AsyncSignal`.
- Reset domains: when the result remains `SyncSignal`, union the reset-domain
  sets from all synchronous contributors.
- If the result becomes `AsyncSignal`, discard reset-domain information. An
  asynchronous value must be synchronized regardless of which reset sources may
  have affected it.

`SyncKind` may remain as a derived helper enum for compatibility, printing, JSON,
and switch statements:

```cpp
enum class SyncKind {
    Sync,
    Clock,
    Reset,
    Async,
};

SyncKind syncKind(const Signal& signal);
```

The important point is that `SyncKind` should not be stored as independent
authoritative state.

## Flop references to domains

Flops should eventually reference global domains directly. This avoids making
every consumer rediscover that a child-local clock name maps to a top-level
input or internally generated clock source.

```cpp
struct FlopInfo {
    std::string name;
    Type type;
    flopType_t flop_type;

    ClockId clock_domain;
    ResetDomains reset_domains;
    std::optional<int> reset_value;

    FlopBinding binding;
};
```

`clock_domain` / `reset_domains` are the semantic global domain references. The
flop datatype should not retain local parsed clock/reset signal names as part of
the final IR.

Current RTL lowering commonly produces zero or one direct reset for a flop, but
the IR should allow multiple reset domains. For example, a flop may be reset by
logic that ORs together two reset sources. In that case, the flop is still in one
clock domain, but has two reset domains.

Multiple reset domains on one flop mean multiple reset sources cause the same
reset action: assigning the single static `reset_value` to the flop. The target
IR does not represent reset/set priority chains or different reset values per
reset source. A frontend that sees a flop where different reset sources force
different values, or where one source behaves as reset and another as set, must
reject it until the IR grows explicit per-reset actions.

The final `FlopInfo` invariants are:

- `reset_domains.ids.empty()` implies `reset_value == std::nullopt`.
- `!reset_domains.ids.empty()` implies `reset_value.has_value()`.
- Every reset domain in `reset_domains` forces the same `reset_value`.

`FlopInfo::type` should be a `Type`, not a `Signal`.

The current IR uses `Signal` as a convenient bundle of name, type, binding, and
domain metadata:

```cpp
struct FlopInfo {
    std::string name;
    Signal type;
    ...
};
```

That is too broad for a flop. A flop is a storage element, not a normal signal.
Its value shape is a `Type`, while its `.d` and `.q` DFG leaves are already
represented by `FlopBinding`.

Current code mostly uses the embedded signal only to access `flop.type.type`.
The other uses are duplicated or obsolete under this rework:

- `flop.type.name` duplicates `FlopInfo::name`.
- `flop.type.binding` duplicates `FlopInfo::binding.q_leaves`.
- `flop.type.clock_domain` and `flop.type.clock_edge` are local-domain metadata
  that should be replaced by global `clock_domain` / `reset_domains` IDs on
  `FlopInfo`.

Therefore the target shape is:

```cpp
struct FlopInfo {
    std::string name;
    Type type;
    flopType_t flop_type;

    ClockId clock_domain;
    ResetDomains reset_domains;
    std::optional<int> reset_value;

    FlopBinding binding;
};
```
