# SystemVerilog `if` and `case` Statement Modifiers: Functional Behavior

**Reference:** IEEE Std 1800-2023, Section 12 — Procedural Programming Statements  
**Scope:** Functional/logical behavior of selection statements and their modifiers (`unique`, `unique0`, `priority`, unmodified). This document is **not** about synthesis implementation strategies; it describes what the standard mandates in terms of observable logical behavior during simulation.

---

## 1. Unmodified `if-else-if`

### 1.1 Basic rule

The `if-else` statement (§12.4) evaluates its condition (`cond_predicate`) and dispatches:

- If the condition evaluates to **true** (nonzero and fully known): the first (`if`) branch executes.
- If the condition evaluates to **false** (zero, `x`, or `z`): the `else` branch executes (if present); otherwise nothing executes.

### 1.2 Chained `if-else-if`: priority semantics

When conditions are chained:

```systemverilog
if (expr0)      stmt0;
else if (expr1) stmt1;
else if (expr2) stmt2;
else            stmt_default;
```

The standard states (§12.4.1):

> "The expressions shall be evaluated in order. If any expression is true, the statement associated with it shall be executed, and this shall terminate the whole chain."

This is **strict priority** evaluation: `expr0` is tested first; only if it is false is `expr1` tested; and so on. The first true branch terminates the search. **If multiple conditions could be simultaneously true, the earliest one in textual order always wins.** No warning is generated — the overlap is silently resolved by priority.

The trailing `else` handles the "none of the above" case. If omitted and no condition is true, no statement executes.

**Functional model:** priority multiplexer chain. The select inputs are tested sequentially; each test gate is only reached if all previous tests were false.

---

## 2. `priority if`

### 2.1 Evaluation order

`priority if` (§12.4.2) evaluates conditions **in the order listed** — identical to unmodified `if-else-if`. This makes the priority intent explicit to the tool, but the functional evaluation rule is the same: first true condition wins.

### 2.2 No-match violation

The key addition over the unmodified form is a **coverage check**: if no condition is true and there is no explicit `else`, the implementation **shall issue a violation report**. If an `else` is present, all values are covered and no violation can occur.

### 2.3 Multi-match behavior

Because evaluation is sequential and stops at the first true condition, multiple true conditions simply result in the first one winning. There is no violation for overlap; `priority` makes no mutual-exclusivity assertion.

**Functional model:** priority multiplexer, identical to unmodified `if-else-if`. The modifier adds only the no-match check.

---

## 3. `unique if`

### 3.1 The asserted constraint

`unique if` (§12.4.2) carries a **designer assertion** that all conditions in the if-else-if chain are **mutually exclusive** — at most one condition is true at any time. Because mutual exclusivity is asserted, the implementation is allowed (though not required) to evaluate conditions in **any order and in parallel**.

The standard states:

> "Unique-if and unique0-if assert that there is no overlap in a series of if–else–if conditions, i.e., they are mutually exclusive and hence it is safe for the conditions to be evaluated in parallel."

### 3.2 Evaluation mechanics

Conditions may be evaluated and compared in any order. The implementation **shall continue** evaluating all conditions even after finding a true one, to check whether any other condition is also true (overlap detection). The standard does not require the tool to evaluate all possible orders — once a violation is found, no further search is required.

### 3.3 No-match violation

`unique if` **shall issue a violation report** if no condition is true and there is no explicit `else`. This is the same requirement as `priority if`.

### 3.4 Multi-match (overlap) violation

If more than one condition is found to be true, the implementation:

1. **Issues a violation report.**
2. **Executes the statement associated with the condition that appears first in the `if` statement** (textual order), and only that statement. Statements for other true conditions are not executed.

> "The implementation shall issue a violation report and execute the statement associated with the true condition that appears first in the `if` statement, but not the statements associated with other true conditions."

**Functional model:** parallel multiplexer with mutually exclusive select conditions. The standard asserts that all conditions are mutually exclusive, making parallel evaluation semantically equivalent to priority evaluation — but the tool verifies this assertion and reports when it is violated.

---

## 4. `unique0 if`

`unique0 if` is identical to `unique if` with one difference: **it does not require that any condition be true**. If no condition matches (and there is no `else`), no violation is reported.

| Property | `unique if` | `unique0 if` |
|---|---|---|
| Mutual exclusivity asserted | Yes | Yes |
| Parallel evaluation permitted | Yes | Yes |
| Overlap (multi-match) → violation | Yes | Yes |
| No match (with no `else`) → violation | **Yes** | **No** |

**Functional model:** Same as `unique if`, but the coverage (full-decode) invariant is relaxed.

---

## 5. Unmodified `case`

### 5.1 Evaluation rule

The `case` statement (§12.5) evaluates the `case_expression` **exactly once**, before any `case_item_expressions`. Then:

> "The case_item_expressions shall be evaluated and then compared in the exact order in which they appear."

This is a **linear priority search**. The first `case_item` whose expression(s) match the `case_expression` wins. A `default` item is skipped during the search; it only fires if all explicit items fail.

The 4-state comparison is exact: every bit must match as `0`, `1`, `x`, or `z`. The `casez` variant treats `z` (and `?`) as don't-cares; `casex` treats both `z` and `x` as don't-cares. In all variants, the evaluation order is still the same sequential priority.

### 5.2 No-match behavior

If no `case_item` matches and there is no `default`: no statement executes. No violation is issued.

**Functional model:** priority multiplexer (first-match-wins linear search).

---

## 6. `priority case`

`priority case` (§12.5.3) imposes the same sequential evaluation order as unmodified `case`, explicitly signaling priority intent. It adds a **no-match violation**: if no `case_item` matches and no `default` is present, the implementation **shall issue a violation report** (or may issue one at compile time if the violation is statically detectable).

**Functional model:** same as unmodified `case`. The modifier adds only the coverage check.

---

## 7. `unique case`

### 7.1 The asserted constraint

`unique case` (§12.5.3) asserts that no two `case_items` overlap — the matching item, if any, is unique. Because of this assertion, case items **may be evaluated in any order** (parallel evaluation is permitted):

> "A priority-case shall act on the first match only. A unique-case and unique0-case assert that there are no overlapping case_items and hence that it is safe for the case_items to be evaluated in parallel."

The `case_expression` is still evaluated exactly once before any `case_item_expressions`.

### 7.2 Evaluation mechanics

> "The case_item_expressions may be evaluated in any order and compared in any order. The implementation shall continue the evaluations and comparisons after finding a matching case_item."

This "continue after finding a match" requirement exists so that the tool can detect whether a second item also matches (overlap detection).

### 7.3 No-match violation

`unique case` **shall issue a violation report** if no `case_item` matches and no `default` is provided.

### 7.4 Multi-match (overlap) violation

If more than one `case_item` matches:

> "The implementation shall issue a violation report and execute the statement associated with the matching case_item that appears first in the case statement, but not the statements associated with other matching case_items."

**Important nuance:** It is NOT a violation for a single `case_item` to list multiple `case_item_expressions` that all match the `case_expression` — that is entirely permitted. The uniqueness constraint is solely about distinct `case_items`, not about the expressions listed within one item.

**Functional model:** parallel multiplexer with mutually exclusive select conditions. The designer asserts mutual exclusivity; the tool verifies it.

---

## 8. `unique0 case`

`unique0 case` is identical to `unique case` except: **if no `case_item` matches and there is no `default`, no violation is reported**.

| Property | `unique case` | `unique0 case` |
|---|---|---|
| Mutual exclusivity of items asserted | Yes | Yes |
| Parallel evaluation permitted | Yes | Yes |
| Overlap (multi-match) → violation | Yes | Yes |
| No match (with no `default`) → violation | **Yes** | **No** |

---

## 9. Violation Reporting Mechanics (§12.4.2.1, §12.5.3.1)

This section documents exactly what the standard mandates about *when* and *how* violations are reported in simulation.

### 9.1 When the check is evaluated

> "A unique, unique0, or priority violation check is evaluated at the time the statement is executed."

The check fires at statement execution time, not at some global quiescent point.

### 9.2 When the report is issued — deferred to the Observed region

Violation *reporting* is **deferred**:

> "...violation reporting is deferred until the Observed region of the current time step (see 4.4)."

The pending violation is placed on a **violation report queue** associated with the currently executing process. This deferral exists specifically to make violation checks immune to zero-delay glitches in the active region set.

### 9.3 Flush mechanism — glitch immunity

A **violation report flush point** is reached if any of the following occurs before the Observed region:

- The procedure, having been suspended due to an event control or `wait`, **resumes execution** (i.e., the process re-triggers).
- The procedure was declared by an `always_comb` or `always_latch` statement and its execution is **resumed due to a transition on one of its dependent signals**.

If a flush point is reached, the violation report queue is cleared — all pending violation reports are **discarded**.

**Consequence:** a glitch that transiently causes an overlap, but resolves before the Observed region, will not produce a false violation report. The violation check is re-evaluated when the process re-triggers, and if the constraint now holds, no violation is reported.

### 9.4 Report maturation

Once the Observed region is reached and the pending violation report has not been flushed, it **matures** and is reported via a tool-specific mechanism. Once matured, it can no longer be flushed.

### 9.5 Example (from the standard)

```systemverilog
always_comb begin
    not_a = !a;
end

always_comb begin : a1
    u1: unique if (a)
        z = a | b;
    else if (not_a)
        z = a | c;
end
```

When `a` transitions from 0→1 while `not_a` is still 1 (mid-glitch), the `unique if` executes with both conditions true → violation is detected and placed in the queue. Before the Observed region, `not_a` updates to 0 and `a1` re-triggers. The re-trigger is a flush point → original violation report is discarded. On re-execution, only one condition is true → no violation. **Net result: no false report.**

---

## 10. Summary Reference Table

| Modifier | Evaluation Order | No-match w/o else/default | Overlap behavior | Parallel eval. permitted |
|---|---|---|---|---|
| *(none)* `if` | Sequential (priority) | No action, no report | First branch silently wins | No |
| `priority if` | Sequential (priority) | **Violation report** | First branch wins (no overlap check) | No |
| `unique if` | Any order | **Violation report** | **Violation report** + first branch executes | **Yes** |
| `unique0 if` | Any order | No report | **Violation report** + first branch executes | **Yes** |
| *(none)* `case` | Sequential (priority) | No action, no report | First item silently wins | No |
| `priority case` | Sequential (priority) | **Violation report** | First item wins (no overlap check) | No |
| `unique case` | Any order | **Violation report** | **Violation report** + first item executes | **Yes** |
| `unique0 case` | Any order | No report | **Violation report** + first item executes | **Yes** |

---

## 11. Key Distinctions for Logic Modeling

### 11.1 Priority vs. parallel

- **Plain and `priority` forms**: the conditions/items are a chain; each stage is only evaluated when all preceding stages have been found false. Logically, this is a **cascaded priority structure** — earlier conditions dominate.
- **`unique` and `unique0` forms**: the designer guarantees mutually exclusive conditions. The conditions/items are logically independent and can be evaluated in parallel. There is no cascaded dominance relationship — the hardware model is a **flat multiplexer with mutually exclusive select conditions**.

### 11.2 Coverage requirement

- `priority` and `unique`: both **require at least one branch to be taken** (no "fall-through" without a report). They differ in their overlap semantics.
- `unique0` and plain: both tolerate the case where no branch is taken.

### 11.3 On overlap: which branch executes?

All four modifiers agree on what executes when multiple conditions are simultaneously true: the branch that appears **first in textual order** in the source code. The difference is only in whether a violation is reported.

### 11.4 `x`/`z` propagation in conditions

For `if` conditions: a condition evaluating to `x` or `z` is treated as **false**. This applies regardless of modifier.

For `case` comparisons (unmodified `case`, `priority case`, `unique case`, `unique0 case`): comparison is exact 4-state — `x` and `z` in the case expression or items are matched literally, not treated as don't-cares. Use `casez`/`casex` with the appropriate modifier for don't-care matching.

---

*End of report. All citations are to IEEE Std 1800-2023.*
