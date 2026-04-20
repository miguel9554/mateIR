# SystemVerilog Section 13 — Tasks and Functions: Support in This Compiler

This document specifies how each feature of IEEE Std 1800-2023 §13 (Tasks and
functions) is handled by this compiler. The compiler targets **synchronous RTL
only** and lowers source into the `Module` IR with a purely combinational
DFG.

All section references below are to IEEE Std 1800-2023.

---

## Fundamental model: automatic subroutines only

The compiler supports subroutines exclusively as **stateless, combinational
abstractions**. At every call site the subroutine body is **inlined** into the
DFG: input arguments become bound expressions, and the return value (or output
arguments for tasks) become DFG nodes in the calling context.

This model maps exactly onto subroutines with **`automatic` lifetime** (§13.3.1,
§13.4.2). `automatic` means all declared items are allocated fresh on each
invocation — no state survives across calls. That property is what makes inlining
semantically correct.

**`static` lifetime is not supported.** Static subroutines share a single copy
of their local variables across all concurrent uses, which is a stateful
simulation concept with no equivalent in a purely combinational DFG.

The `automatic` keyword is **required** on every subroutine declaration; a
subroutine that omits it defaults to `static` (per §13.3.1 / §13.4.2), which
is rejected.

---

## 13.2 Overview

The general task/function distinction (tasks may contain timing controls and
enable other tasks; functions execute combinationally and return a value) maps
directly to the compiler's model:

- **Functions** are the primary use case: they compute and return a value,
  translating to a pure combinational cone inlined at each call site.
- **Automatic tasks** with only `input`/`output` arguments are also supported:
  they behave identically to a function with multiple return channels. The call
  site drives input arguments in and receives output arguments back.

Any aspect of the spec that depends on simulation time or sequential process
semantics has no meaning in a combinational DFG and is **not supported**.

---

## 13.3 Tasks

### Supported

A task declared `automatic` with a port list of `input` and/or `output`
arguments (pass by value) is supported. The task body must consist solely of
combinational procedural statements (assignments, conditionals, loops with
static bounds) — the same subset accepted inside `always_comb` blocks.

```systemverilog
task automatic split(input logic [7:0] x, output logic [3:0] hi, lo);
    hi = x[7:4];
    lo = x[3:0];
endtask
```

Both ANSI-style (ports in parentheses) and non-ANSI-style (ports as separate
declarations) are accepted.

The `return` statement is supported to exit early.

### Not supported

| Feature | Reason |
|---|---|
| `static` lifetime (default when `automatic` is absent) | Holds state across invocations — not synthesisable |
| Timing controls inside task body (`#`, `@`, `wait`, `fork-join`) | No concept of time in the DFG |
| Enabling (calling) other tasks from within a task body | Supported only when the called task is itself a valid automatic task per these rules |
| `inout` arguments | Bidirectional copy-in/copy-out has no direct combinational equivalent |
| `ref` arguments (§13.5.2) | Pass-by-reference requires a memory model |
| `const ref` arguments | Same — no reference semantics |
| Tasks inside `interface` or `program` scope | Those constructs are not supported |

### 13.3.1 Static and automatic tasks

Only `automatic` is supported. `static` is rejected. See the fundamental model
section above.

### 13.3.2 Task memory usage and concurrent activation

Static storage and concurrent activation are simulation concepts irrelevant to
combinational inlining. Not supported; not applicable.

---

## 13.4 Functions

### Supported

A function declared `automatic` that returns a value of any supported data type
(§6) is supported. The function body must consist solely of combinational
procedural statements.

```systemverilog
function automatic logic [7:0] byte_reverse(input logic [7:0] x);
    byte_reverse = {x[0], x[1], x[2], x[3], x[4], x[5], x[6], x[7]};
endfunction
```

Both return mechanisms are supported:
- Assigning to the implicit variable named after the function.
- The `return expression;` statement.

`void` functions (called as a statement rather than an expression) are
supported when the body has no side effects beyond writing its output
arguments.

Both ANSI-style and non-ANSI-style port declarations are accepted.

### 13.4.1 Return values and void functions

Non-void return values of any supported data type (packed integers, structs,
enums — per §6) are supported. The return type appears in the DFG as the result
node of the inlined function subgraph.

`void` functions are supported; they are treated like an automatic task with
no return value.

Calling a non-void function and discarding its return value via `void'(...)` is
accepted.

Recursive function calls are **not supported**. A function body may not contain
a direct or indirect call to itself. Unrolling arbitrary recursion to a static
combinational depth is not implemented.

### 13.4.2 Static and automatic functions

Only `automatic` is supported. `static` is rejected. See the fundamental model
section above.

### 13.4.3 Constant functions

Constant functions (§13.4.3) are evaluated at elaboration time when all
arguments are constant expressions, yielding a compile-time constant result.
They are commonly used to compute parameter-derived widths (e.g. `clogb2`).

**Not yet implemented.** A constant function call at elaboration time (e.g. in
a `localparam` or dimension expression) is not yet supported. Calls to
user-defined functions in non-runtime contexts will produce an error.

### 13.4.4 Background processes spawned by function calls

`fork-join_none` inside a function, and any construct that spawns a background
process, are **not supported**. There is no process model in the compiler.

---

## 13.5 Subroutine calls and argument passing

### 13.5.1 Pass by value

**Supported.** All `input` and `output` arguments are passed by value. At the
call site, each `input` expression is bound to the corresponding formal; each
`output` formal's final value is written back to the corresponding actual
variable at the end of the inlined body.

The order of evaluation of argument expressions is the same as the DFG
evaluation order and is fully determined by data flow.

### 13.5.2 Pass by reference (`ref`)

**Not supported.** The `ref` and `const ref` argument qualifiers have no
equivalent in a purely combinational DFG. Any use of `ref` in a subroutine
declaration or call is an error.

### 13.5.3 Default argument values

**Not yet implemented.** Declaring a formal with a default expression
(`input int x = 0`) is not yet supported. All arguments at each call site must
be supplied explicitly.

### 13.5.4 Argument binding by name

**Not yet implemented.** Named argument passing (`.argname(expr)` syntax) is
not yet supported. All arguments must be supplied positionally.

### 13.5.5 Optional argument list

Empty parentheses `()` on a call to a task or void function that declares no
arguments (or where all arguments have defaults) are not yet tested. Behaviour
follows from §13.5.3 status.

---

## 13.6 Import and export functions (DPI)

**Not supported.** DPI (`import "DPI-C"` / `export "DPI-C"`) is a foreign
language interface that has no meaning in a synthesisable RTL compiler.

---

## 13.7 Task and function names

Task and function names follow the same scoping rules as other identifiers,
with the forward-reference exception described in §23.8.1 (a call may precede
the subroutine definition in the same scope). This is supported via slang's
elaboration order.

Package-qualified calls (`pkg::func(...)`) are supported when the function is
declared inside a package that has been imported.

---

## 13.8 Parameterized tasks and functions

**Not supported.** Parameterized subroutines are expressed via static methods
in parameterized classes (§8.10, §8.25), and classes are not supported.

---

## STATUS

### Implemented

| Feature | Notes |
|---|---|
| `automatic` function declarations (ANSI and non-ANSI style) | Body elaborated as combinational DFG; inlined at call sites |
| `automatic` task declarations with `input`/`output` ports | Treated identically to functions with multiple outputs |
| Non-void return types (packed integers, structs, enums) | Return node becomes the inlined result in the calling DFG |
| `void` functions called as statements | Inlined with no result node |
| `return expr;` statement and implicit function-name variable | Both return mechanisms supported |
| `input` and `output` arguments (pass by value) | Input bound at call site; output written back |
| Package-qualified calls (`pkg::func(...)`) | Resolved via `PackageRegistry` |

### Deferred (not yet implemented)

| Feature | Reason |
|---|---|
| Constant functions (§13.4.3) | Elaboration-time call evaluation not yet implemented |
| Default argument values (§13.5.3) | No current test exercises this |
| Named argument binding (§13.5.4) | No current test exercises this |
| Recursive functions | Requires elaboration-time unrolling to a fixed depth |

### Not supported (out of scope)

| Feature | Reason |
|---|---|
| `static` lifetime subroutines | Hold state — not synthesisable in this model |
| Time-controlling statements inside subroutines | No time model |
| `inout` arguments | No equivalent in combinational DFG |
| `ref` / `const ref` arguments | No reference semantics |
| DPI import/export (§13.6) | Foreign language interface; not synthesisable |
| Parameterized subroutines via class static methods (§13.8) | Class support not implemented |
| `fork-join` constructs inside subroutines (§13.4.4) | No process model |
