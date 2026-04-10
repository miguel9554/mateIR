# DFG Mux IR Specification

## Summary

The DFG has a single `MUX` op.

- `if` / ternary lower to binary `MUX` nodes
- `if / else-if / else` lowers to chains of binary `MUX` nodes
- `case(sel)` lowers to one exhaustive value-decoded `MUX`

This replaces the old split between `MUX` and `MUX_N`.

## Rationale

The old `MUX_N` encoding represented a case-like select as positional halves in
the generic `in` array:

- selectors in the first half
- data expressions in the second half

That encoding made the semantics implicit and forced passes to recover structure
 from operand position. It also modeled `case` as a set of one-hot guards
instead of what the IR actually needs: a total mapping from selector codes to
result expressions.

The unified `MUX` design makes the important semantics explicit:

- one selector expression
- one data arm per selector code
- exhaustive coverage of the selector domain

## Semantics

A `MUX` represents a total combinational function of a selector `sel`.

If `sel` has width `S`, then the mux has exactly `2**S` selector codes. The IR
must define the output for every code in `[0, 2**S - 1]`.

There is no `default` concept in the IR.

A source-level `default` branch is only elaboration sugar for:

- every selector code not covered explicitly by the source `case`
- mapping to the same result expression

Multiple selector codes may map to the same data expression. The IR does not
model that as a tie or aliasing primitive; it is simply repeated mapping.

## Representation

`MUX` uses:

- `in[0]` as the selector input
- `in[1..]` as the data arms
- `mux_values` as metadata parallel to `in[1..]`

So arm `i` means:

- selector code `mux_values[i]`
- data expression `in[i + 1]`

The DFG validator enforces:

- at least two data arms
- `mux_values.size() == in.size() - 1`
- unique selector codes
- exhaustive coverage for the selector width

## Lowering Rules

### Ternary and `if`

`cond ? a : b` and `if (cond) a else b` lower to a binary `MUX`:

- selector width is 1
- selector code `1` maps to the true branch
- selector code `0` maps to the false branch

An `if / else-if / else` chain is priority logic, so it lowers to nested binary
`MUX` nodes. Priority is expressed by the graph structure, not by a distinct mux
kind.

### `case`

`case(sel)` lowers to one exhaustive value-decoded `MUX`.

For combinational lowering, every selector code must resolve to a concrete
expression by elaboration. Uncovered codes are filled from:

- `default`, if present
- otherwise a value already established earlier in the same procedural flow

If neither exists, elaboration fails because the IR does not permit unspecified
combinational selector codes.

For sequential lowering, uncovered codes may resolve to retained `.q`.

## Unsupported Semantics

The IR does not yet model:

- don’t-care / X selector coverage
- guarded-arm exclusive muxes
- unique/unique0 violation semantics

Because of that:

- `unique if`
- `unique0 if`
- `unique case`
- `unique0 case`

are rejected during elaboration.

`priority case` is accepted and treated the same as plain `case` because this IR
does not model simulation-time priority checks; it only needs the resulting
deterministic value mapping.

## Future Extension

If the compiler later needs explicit don’t-care support or exclusive guarded
selection semantics, that should be added as a new generalized guarded-arm mux
representation. This design intentionally keeps the current `MUX` limited to an
exhaustive value-decoded function.
