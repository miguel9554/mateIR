# DFG Mux Design Notes

This note is historical context from before the mux unification. The current
design is specified in `docs/dfg_mux_ir_spec.md`.

This note captures the current design discussion around `MUX` and `MUX_N` in
the DFG so it can be resumed later without reconstructing the reasoning.

## Current State

- The DFG currently has two mux-like ops:
  - `MUX`: 2-way conditional select
  - `MUX_N`: N-way one-hot style select
- In elaboration today:
  - ternary expressions lower to `MUX`
  - `if`-style control flow lowers to `MUX`
  - `case` should lower to `MUX_N`

## Why A Unified Mux Is Attractive

The appealing future direction is a single generalized mux concept instead of
two separate ops. A 2-way mux is just a special case of a multi-arm mux, so
from an IR-design perspective the split is mostly representational.

The preferred conceptual representation from this discussion is:

- one mux node
- represented as a list of guarded arms
- not as positional selector/data halves

That is preferable to the current `MUX_N` encoding because it is easier to map
from control flow and avoids overloading vector position with hidden meaning.

## Important Nuances From The Discussion

### 1. "Default" is not an IR primitive

A source-level `default` branch is only RTL syntax sugar. It should not become a
special semantic concept in the DFG.

At IR level, what matters is only the resulting combinational function.

Examples:

- `if (c) x = a; else x = b;`
  - functionally complete from the two explicit outcomes
- a `case` with all values covered
  - functionally complete from its arms alone
- a bare `if` or partial `case`
  - not complete unless surrounding semantics provide the remaining value

So the notion to preserve is not "default branch", but whether the arm set is
exhaustive or whether some residual value must come from surrounding semantics.

### 2. A mux should not require an always-present fallback

The discussion established that a generalized mux should not force an explicit
"fallback"/"else" operand in all cases.

For example, for a 2-bit selector with explicit arms for `00`, `01`, `10`, and
`11`, there is no residual case. Requiring a fallback in that representation
would be artificial and misleading.

So a future unified mux representation should allow:

- exhaustive arm sets with no residual branch
- non-exhaustive arm sets where elaboration must provide the residual value

### 3. Priority vs unique/exclusive

The difference only matters when more than one guard can be true at the same
time.

- If guards are mutually exclusive, priority and exclusive semantics compute the
  same function.
- If guards overlap, a priority mux has deterministic "first match wins"
  behavior, while an exclusive mux assumes that overlap should not happen.

For this compiler, the discussion concluded:

- preserving a semantic distinction may be useful later
- but it is not obviously required unless later passes need to exploit or check
  exclusivity
- at minimum, the IR should not accidentally force `case` into a priority chain
  if the intended representation is a multi-way select

## Practical Conclusion For Now

- Keep `MUX` and `MUX_N` distinct for now.
- `if` lowering should emit `MUX`.
- `case` lowering should emit `MUX_N`.
- Do not represent `case` as a cascade of `MUX` nodes.

This keeps current semantics explicit while leaving open the option of a future
single generalized mux op once the DFG representation itself is cleaned up.
