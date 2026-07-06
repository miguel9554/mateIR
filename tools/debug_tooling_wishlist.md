# Debug Tooling Remaining Work

Friction points identified during ibex_core and array_indexing validate debug sessions
(2026-06-08).

This document lists only gaps that are still not implemented. Already-landed
`dfg_inspect.py` commands and the basic single-signal VCD timeline diff are omitted.

The motivating compiler bug involved SLICE nodes, but the required tooling should not be
SLICE-specific. The same observability problems apply to MUX arm selection, arithmetic
widening, signed comparisons, CONCAT layout, and future DFG operations.

## Design Principle

Provide generic ways to answer these questions for any DFG node:

1. What exactly is this node, including all semantic type and operation metadata?
2. Where did it come from, and how did passes transform it?
3. What consumes it and what does it depend on?
4. Which operation contract or invariant is violated?
5. During simulation, which inputs, decisions, and result did the operation use?

An operation may expose operation-specific facts, but those facts should be presented
through common artifact, validation, inspection, and tracing interfaces. Do not add
one-off commands such as `slices` or simulator flags such as `--debug-slices`.

---

## Complete DFG artifacts

**Problem**: DFG JSON is not a lossless description of the semantic information carried by
the in-memory DFG. For example, `DFG::renderJson` omits `packed_dims`, causing a node that
uses packed-dimension semantics at runtime to appear flat in the artifact.

The same class of omission can affect any operation whose behavior depends on types,
input roles, operation attributes, or lowering origin.

**Proposal**: Define one complete debug serialization for every DFG node:

- full `Type`, including kind, width, signedness, packed dimensions, and unpacked dimensions
- node name, instance path, source location, and stable debug identity
- named input edges rather than only positional input ids
- operation attributes, such as MUX selector values
- lowering provenance and the pass that created or last rewrote the node

Example:

```json
{
  "id": 117,
  "debug_id": 8421,
  "op": "SLICE",
  "type": {
    "kind": "integer",
    "width": 7,
    "signed": false,
    "packed_dims": []
  },
  "inputs": [
    {"role": "source", "node": 58},
    {"role": "high", "node": 61},
    {"role": "low", "node": 62}
  ],
  "provenance": {
    "origin": "SimpleRangeSelect",
    "created_by": "elaboration"
  }
}
```

The exact serialized fields should be driven by the in-memory node and type structures.
The JSON serializer must not maintain a separate, incomplete interpretation of them.

**SLICE validation case**: `packed_dims` and lowering origin become visible without adding
SLICE-specific output paths.

**Impact: Critical** — all offline inspection depends on artifact fidelity.

---

## Generic node inspection and graph queries

**Problem**: Existing commands answer several useful fixed questions, but investigating a
new operation frequently requires a new command or manual JSON searches.

**Proposal**: Extend `dfg_inspect.py` around generic selectors, filters, and traversal:

```text
dfg_inspect.py <file> node <id-or-name> [--details]
dfg_inspect.py <file> nodes [--op OP] [--name PATTERN] [--at FILE:LINE]
dfg_inspect.py <file> uses <id-or-name> [--op OP] [--depth N] [--details]
dfg_inspect.py <file> deps <id-or-name> [--op OP] [--depth N] [--details]
dfg_inspect.py <file> neighborhood <id-or-name> --fanin N --fanout N
```

`node --details` should show the complete serialized node:

- full type
- named inputs and their types
- operation attributes
- provenance
- direct consumers

`uses` and `deps` generalize operation-specific consumer commands. For example:

```text
dfg_inspect.py <file> uses nzb_load.q --op SLICE --details
dfg_inspect.py <file> uses csr_addr --op MUX --details
dfg_inspect.py <file> deps result --op CONCAT --depth 3
```

The output may include operation-specific attributes, but selection and traversal remain
generic.

**SLICE validation case**: `uses nzb_load.q --op SLICE --details` replaces the proposed
special-purpose `slices nzb_load.q` command.

**Impact: High** — provides reusable investigation primitives for existing and future ops.

---

## Shared operation contracts and generic validation

**Problem**: A Python `lint` implementation that reproduces SLICE index arithmetic would
duplicate simulator semantics and only catch one class of bug. Similar duplication would be
needed for MUX, arithmetic, comparisons, and future operations.

**Proposal**: Make operation contracts explicit in C++ and validate every DFG operation
through the existing DFG validation path.

Each operation contract should cover relevant invariants such as:

- required input roles and input count
- required constant inputs
- input and output type compatibility
- legal widths and ranges
- operation-specific semantic constraints

Nontrivial derived semantics should live in shared helpers used by both validation and
simulation. For example, the helper that resolves a source-space selection into an internal
bit range should be used by the simulator and by DFG validation rather than reimplemented
in `dfg_inspect.py`.

Validation diagnostics should identify:

- node id, op, name, and source location
- violated contract
- relevant input nodes, values, and types
- derived values that caused the failure

Example:

```text
ERROR [117] SLICE @ array_indexing.v:100
  contract: selected internal range must be within source width
  source=[58] INPUT nzb_load.q type=Integer[7:1]
  inputs: high=0 low=0
  derived: internal_low=-1 internal_high=-1 source_width=7
```

The validation entry point should operate on all nodes. Filtering diagnostics by operation,
name, or source location is useful, but validation itself should not be operation-specific.

**SLICE validation case**: the negative effective bit position is caught by the same
contract framework that can catch invalid MUX arms or incompatible arithmetic inputs.

**Impact: High** — turns debug-time suspicions into compiler-enforced invariants.

## Cross-pass node and cone diff

**Problem**: Existing per-pass dumps show each graph independently, but it is difficult to
answer when a node lost metadata, changed type, changed driver, or was replaced.

**Proposal**: Enhance `dfg_inspect.py diff` using stable debug identity and provenance:

```text
dfg_inspect.py <before.json> diff <after.json> [--node ID-OR-NAME] [--cone] [--details]
```

Report changes to:

- operation and operation attributes
- full type
- named input edges
- instance path and source location
- provenance
- node creation, replacement, and removal

**SLICE validation case**: this identifies the exact pass where `packed_dims` or index
convention metadata changed. It is equally useful for any pass-induced DFG regression.

**Impact: High** — makes per-pass artifacts useful for locating the responsible transform.

---

## Unified VCD diff tool

**Problem**: We need both single-signal timeline comparison and scope-level first-divergence
analysis. They share parsing, timescale normalization, path resolution, and comparison logic.

**Proposal**: Evolve the existing signal diff into one unified `tools/vcd_diff.py`:

```text
python3 tools/vcd_diff.py signal <vcd1> <signal1> <vcd2> <signal2>
python3 tools/vcd_diff.py signal <vcd1> <vcd2> --signal addr_incr_two
python3 tools/vcd_diff.py scope  <vcd1> <scope1> <vcd2> <scope2> [--signals]
```

`signal` mode should support exact, case-insensitive, suffix, and unique bare-name
resolution. `scope` mode should compare matched signals and sort them by first divergence.

**Impact: High** — quickly identifies the earliest externally visible divergence, which can
then be investigated using the generic DFG artifact and trace tools.

---

## Remaining priority table

| Area | Addition | Impact |
|------|----------|--------|
| DFG JSON | complete semantic node/type serialization | **Critical** |
| DFG validation | shared operation contracts and derived-semantics helpers | **High** |
| simulator | generic filtered structured DFG evaluation trace | **High** |
| dfg_inspect.py | generic node selectors, `uses`, `deps`, and neighborhood queries | **High** |
| dfg_inspect.py | stable-identity cross-pass node and cone diff | **High** |
| unified VCD diff tool | scope first-divergence and bare-name signal modes | **High** |
