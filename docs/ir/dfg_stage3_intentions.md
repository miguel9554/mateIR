# Deferred DFG Stage 3 Intent

This note captures the intended follow-up after the control-flow refactor in
`src/passes/elaboration.cpp`.

## Intention

Tighten the DFG node API so operand structure is no longer expressed as a
public, freely mutable `DFGNode::in` vector with op-specific meaning encoded by
convention.

## Rationale

- `DFGNode::in` currently mixes several semantics:
  - positional operands for arithmetic / mux / index nodes
  - driver edges for `OUTPUT` and `SIGNAL`
  - aggregate membership for unpacked-array placeholder nodes
- Elaboration and later passes directly mutate those vectors, which makes
  invariants implicit and easy to violate.
- `MODULE` nodes also rely on parallel arrays (`in` and `input_names`) with no
  single validated representation.
- The control-flow refactor improves elaboration structure, but it intentionally
  leaves the DFG representation unchanged to keep the behavioral change set
  small and regression-safe.

## Intended Direction

- Make driver-style connectivity explicit instead of encoding it as `in.size()`
  being `0` or `1`.
- Separate aggregate membership from operand lists.
- Add typed helpers / accessors for structured ops such as `MUX` and `INDEX`.
- Validate structural invariants at the DFG API boundary instead of relying on
  downstream passes to rediscover malformed shapes.

## Non-Goals For The Current Change

- No IR class hierarchy rewrite.
- No semantic changes to optimization passes.
- No attempt to migrate all current DFG node kinds in the same patch as the
  control-flow refactor.
