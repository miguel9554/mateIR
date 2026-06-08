---
name: testcase-rules
description: RTL and TB coding rules for compiler testcases. Use this when writing, modifying, or reviewing RTL or a testbench in any test under tests/ — including deciding whether a handwritten TB is needed and validating that it follows the repo's driving rules.
---

## Testbench driving rules

When a handwritten testbench is needed:
- each async signal is driven from its own `initial` block
- timing delays are only allowed in those async-driving `initial` blocks
- for each clock domain, all synchronous driving belongs in that domain's `always @(posedge clk)` block
- no timing delays are allowed inside synchronous driver blocks
- synchronous driving should use NBA assignments

## RTL requirements

- the top RTL under test must not be purely combinational
- it must contain flops, including flops that register all outputs
- it must contain at least one flop with async reset and at least one flop without async reset
- if needed, duplicate the flop structure and vary only the presence or absence of async reset so both cases are exercised cleanly

## `unique case` contract

When a testcase intentionally uses `unique`-qualified `case`, enforce the `unique` contract in RTL instead of relying on TB stimulus to honor it:
- derive sanitized internal signals before the `unique case` so uncovered and overlapping top-level input patterns are collapsed into legal ones
- for `unique case (1'b1)` style constructs, ensure at most one sanitized item can be true at a time
- for `unique case (sel)` style constructs, ensure the sanitized selector is always mapped into the covered value set
- do not rely on handwritten TB constraints to keep `unique` inputs legal; the testcase RTL itself should enforce that property
- only drive illegal `unique` scenarios directly if the testcase is explicitly about undefined-behavior handling, and document that intent clearly

## Async reset driving rule

Verilator does not fire the async-reset sensitivity until it observes a proper unasserted→asserted edge. If the reset starts asserted at time 0 (no prior transition), Verilator never triggers the always_ff and the flop stays at its Verilator-initial value (0) instead of the reset value — causing a mismatch against the custom-sim, which evaluates correctly from time 0.

Always drive async resets with an explicit unasserted→asserted transition:
```
// negative-polarity reset example
initial begin
    _if.rst_n = 1'b1;   // start UNASSERTED
    #1 _if.rst_n = 1'b0; // assert after 1 ns — Verilator sees the 1→0 edge
    #12 _if.rst_n = 1'b1;
end
```
For positive-polarity resets, start at `1'b0` and transition to `1'b1`. The leading unasserted hold time should be small (1 ns is canonical) but must be non-zero.
