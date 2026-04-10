# SystemVerilog Section 6 — Data Types: Support in This Compiler

This document describes how each feature of IEEE Std 1800-2023 §6 (Data types) is
handled by this compiler. The compiler targets **synchronous RTL only**: it accepts
synthesisable, clocked Verilog as input and lowers it into the `ResolvedModule` IR.
Many features in §6 exist solely for simulation, verification, or analogue modelling
and are therefore not supported.

All section references below are to IEEE Std 1800-2023.

---

## Fundamental model distinction: nets vs variables (§6.5)

SystemVerilog distinguishes two kinds of data objects:

- **Nets** (`wire`, `tri`, `wand`, …): structural connections driven by one or more
  continuous assignments or primitive outputs. Their value is resolved from all drivers.
- **Variables** (`logic`, `reg`, `bit`, `int`, …): storage elements driven by at most
  one procedural block or one continuous assignment.

**This compiler accepts only the variable model.** Every signal in the IR is a
variable — a named storage element whose value is written by exactly one assignment
at any logical point in time. Nets (and everything that depends on net-resolution
semantics) are not supported.

In source Verilog, `wire` is accepted syntactically in port or signal declarations
where it is used purely as a connection label (single driver, no strength, no
multi-driver resolution), but it is elaborated identically to a `logic` variable.
Using `wire` in any other net-modelling way is an error.

---

## 6.3 Value set

### 6.3.1 Logic values

SystemVerilog defines four basic values: `0`, `1`, `x` (unknown), and `z`
(high-impedance).

**Supported values: `0` and `1` only.**

The compiler IR is **2-state**: all signal values are `0` or `1`. The `x` and `z`
values are not tracked and do not propagate through the DFG. Source-level constructs
that would produce `x` or `z` (e.g. uninitialised registers, high-impedance outputs)
are unsupported.

Numeric literals using `x` or `z` digits (e.g. `4'b10x0`, `8'hzz`) are not
supported in synthesisable RTL context.

### 6.3.2 Strengths

Drive strengths (`strong0`, `weak1`, `supply0`, etc.) and charge strengths
(`small`, `medium`, `large`) are **not supported**. They are exclusively a
net-modelling concept.

---

## 6.4 Singular and aggregate types

Both singular and aggregate types are supported. The distinction matters primarily
for operators and functions that behave differently on scalars vs collections; see
§7 for aggregate type details (arrays, structs, unions).

---

## 6.6 Net types

All built-in net types are **not supported**:

| Net type | Status |
|---|---|
| `wire` | Accepted only in port/signal declarations as a synonym for `logic` (single-driver, no strength). All net semantics are discarded. |
| `tri` | Not supported |
| `wand`, `triand` | Not supported |
| `wor`, `trior` | Not supported |
| `trireg` | Not supported |
| `tri0`, `tri1` | Not supported |
| `supply0`, `supply1` | Not supported |
| `uwire` | Not supported |
| User-defined `nettype` | Not supported |
| `interconnect` | Not supported |

There is no multi-driver resolution, no strength modelling, and no `force`/`release`
semantics.

---

## 6.7 Net declarations

Net declarations (§6.7) are **not supported** except for the degenerate case
described in §6.6 (`wire` used as a `logic` alias in a port declaration).

Attributes of net declarations that are not supported:
- Drive strength specifications
- Charge strength specifications (`trireg`)
- Delay specifications (`#delay`)
- Multiple-driver declarations
- `vectored` / `scalared` qualifiers
- User-defined nettype declarations

---

## 6.8 Variable declarations

Variable declarations are the **primary mechanism** for declaring signals.

Supported declaration forms:
```systemverilog
logic [7:0]        data;
logic              flag;
logic signed [31:0] value;
reg   [3:0]        nibble;
```

Not supported in variable declarations:
- `var` keyword (redundant in RTL context; ignored if present)
- `automatic` / `static` lifetime qualifiers (all module-level signals are static)
- Initialiser expressions (e.g. `logic [7:0] x = 8'hFF`): not supported; the
  compiler throws an error. Reset logic must be expressed via explicit flop
  reset conditions in an `always` block.

---

## 6.9 Vector declarations

Packed vector types are fully supported. A vector is a multi-bit signal declared
with a range specifier:

```systemverilog
logic [7:0]  byte_val;   // 8-bit vector, unsigned
logic signed [15:0] sx;  // 16-bit vector, signed
```

Both `[msb:lsb]` (descending, `msb ≥ lsb`) and `[lsb:msb]` (ascending) range
forms are accepted. The compiler normalises all ranges internally.

The `vectored` and `scalared` access qualifiers (§6.9.2) are not supported and
will cause an error if present.

---

## 6.10 Implicit declarations

**Not supported.** All signals must be explicitly declared. The compiler never
creates an implicit net for an undeclared identifier. Any reference to an undeclared
name is a compile error.

The `` `default_nettype `` compiler directive is not supported.

---

## 6.11 Integer data types

This compiler has two distinct contexts for data types that have different rules:

- **RTL signals**: inputs, outputs, internal signals, and flops — the objects that
  live on the DFG. These must have explicit widths.
- **Parameters** (`parameter`/`localparam`): elaboration-time metaprogramming values
  that are folded away before the DFG is built. These may have richer types.

### RTL signal types

Only vector types with explicit user-defined widths are supported for RTL signals:

| SV type | Signedness | Support |
|---|---|---|
| `logic [N:0]` | unsigned by default | **Supported** — primary RTL type |
| `reg [N:0]` | unsigned by default | **Supported** — synonym for `logic` in this compiler |
| `bit [N:0]` | unsigned | **Supported** — treated identically to `logic` (IR is always 2-state; the 2-state vs 4-state distinction is erased) |
| `byte`, `shortint`, `int`, `longint`, `integer` | — | **Not supported** — throws an error. Use `logic [N:0]` with an explicit width instead. |
| `time`, `realtime` | — | **Never supported**, not even for parameters |

The `signed`/`unsigned` qualifiers are respected and affect arithmetic operator
semantics (sign extension, comparison).

All RTL signal types are stored as `ResolvedType` with `kind = Integer`. The
`is_signed` flag in `ResolvedIntegerInfo` records the declared signedness.

### Parameter types

Parameters live in an elaboration-time metaprogramming space separate from the DFG.
They may use any integer type (including `byte`, `shortint`, `int`, `longint`,
`integer`), `real`/`shortreal`, `string`, and user-defined types. See §6.12, §6.16,
and §6.20 for further detail. `time` and `realtime` are never supported.

---

## 6.12 Real, shortreal, and realtime data types

`real` and `shortreal` are **supported as parameter types**. They are not supported
as RTL signal types (not synthesisable).

`realtime` is **never supported** (simulation time concept; no hardware meaning).

---

## 6.13 Void data type

**Not supported.** There are no functions or tasks with return values in the RTL
model (procedural blocks do not return values in the synthesisable subset).

---

## 6.14 Chandle data type

**Not supported.** This is a DPI/simulation construct with no hardware meaning.

---

## 6.15 Class

**Not supported.**

---

## 6.16 String data type

**Supported as a parameter type.** Not supported as an RTL signal type.

---

## 6.17 Event data type

**Not supported.** Events are a simulation-synchronisation construct. Clock and
reset signals are expressed through the dedicated `.domains.yaml` metadata.

---

## 6.18 User-defined types (`typedef`)

`typedef` is **supported** for any type that is itself supported in the relevant
context (RTL signal or parameter):

```systemverilog
typedef logic [7:0]          byte_t;
typedef logic signed [31:0]  word_t;
typedef enum logic [1:0] { IDLE, FETCH, EXEC, DONE } state_t;
typedef real                 gain_t;   // valid in parameter context
```

**Forward typedefs** (`typedef enum state_t;`, `typedef struct foo_t;`, etc.) are
**supported**. They are necessary for the scope resolution system to handle
mutually-referential or out-of-order type declarations.

Interface-based typedefs (`typedef p.data_t my_t;`) are not supported.

---

## 6.19 Enumerations

Enumerations are **supported** when the base type is an integer type. Each
`enum` is stored in the IR as a `ResolvedType` with `kind = Enum`, carrying the
member names and their integer values in `ResolvedEnumInfo`.

Supported forms:
```systemverilog
typedef enum logic [1:0] { IDLE=0, RUN=1, DONE=2 } state_t;
typedef enum { A, B, C } simple_t;   // implicit int base type
```

Restrictions:
- The base type must be a supported integer type (see §6.11 RTL signal types).
- Member values must be elaboration-time constants (literals, `parameter`,
  `localparam`, or constant expressions of these).
- Enum values with `x` or `z` bits are not supported (consistent with the 2-state
  value set).

Enum type methods (`.first()`, `.last()`, `.next()`, `.prev()`, `.num()`,
`.name()`) are **supported**.

---

## 6.20 Constants

### `parameter` and `localparam`

**Supported.** Parameters and local parameters are used to express elaboration-time
constants, most commonly for signal widths and other structural integers.

```systemverilog
parameter int WIDTH = 8;
localparam int DEPTH = 1 << WIDTH;
```

Parameters live in a metaprogramming space separate from the DFG and support a
richer set of types than RTL signals. Supported parameter types include:
- All integer types: `logic [N:0]`, `bit [N:0]`, `int`, `byte`, `shortint`,
  `longint`, `integer`
- Floating-point: `real`, `shortreal`
- `string`
- User-defined types (via `typedef`, including `parameter type T`)

`time` and `realtime` are never supported, not even for parameters.

Parameter overrides via the `#(…)` instantiation syntax are supported.

### `specparam`

**Not supported.** Specify parameters are timing-related and have no role in the
synchronous IR.

### `const`

**Not supported** inside procedural blocks (there is no supported procedural context
in RTL). `const` in the class or function sense is irrelevant.

### `$` constant

**Not supported** (queue / unbounded-range contexts are not relevant to the RTL model).

---

## 6.21 Scope and lifetime

All module-level signals have **static lifetime** and **module scope**. The
compiler does not distinguish static vs automatic lifetimes (those apply to tasks
and functions, which are not part of the synthesisable subset handled here).

Scoping beyond the module level (package scope, compilation-unit scope, and the
rules governing identifier resolution across scopes) is supported. See
`docs/sv/sv_packages.md` for the full specification.

---

## 6.22 Type compatibility

Type compatibility rules (matching, equivalent, assignment-compatible) apply during
elaboration. The key rules in effect:

- A `typedef` alias is **matching** to its underlying type.
- Two packed vectors with the same total width, same 4-state/2-state category (erased
  here — everything is 2-state), and same signedness are **equivalent**.
- Integer types of different widths are **assignment-compatible**: the compiler will
  truncate or extend (zero-extend for unsigned, sign-extend for signed) as specified
  by §10.7.

Enum types are **not** assignment-compatible with arbitrary integer expressions
without an explicit cast; an integer cannot be implicitly assigned to an enum
variable.

---

## 6.23 Type operator

**Not currently supported (TBD).** The `type(expr)` operator enables type-based
`generate` dispatch and parameterised type references. Support is planned but not
yet implemented due to the complexity of integrating it with the elaboration and
scope systems.

---

## 6.24 Casting

Size and signedness casts appearing in RTL expressions are **supported**:

```systemverilog
8'(wide_signal)          // truncate to 8 bits
signed'(unsigned_val)    // reinterpret as signed
17'(a + b)               // widen result to 17 bits
```

These are lowered into DFG nodes (sign-extend, zero-extend, truncate) during
elaboration.

The `$cast` dynamic cast system task is **not supported**.

Bit-stream casting (§6.24.3) between aggregate types is **not supported**.

---

## 6.25 Parameterized data types

**Not supported.** Parameterised data types are implemented via class
parameterisation (`class C #(parameter …)`), which is not supported.

---

## Summary table

| §6 feature | Support |
|---|---|
| 2-state values (0, 1) | **Yes** |
| 4-state values (x, z) | **No** — erased; IR is 2-state |
| Drive/charge strengths | **No** |
| Nets (`wire`, `tri`, `wand`, …) | **No** (bare `wire` port accepted as `logic` alias) |
| User-defined nettypes | **No** |
| `interconnect` net | **No** |
| Variable declarations | **Yes** |
| Packed vector declarations (`logic [N:0]`) | **Yes** |
| Implicit net declarations | **No** |
| `logic`, `reg`, `bit` (RTL signals) | **Yes** |
| `byte`, `shortint`, `int`, `longint`, `integer` (RTL signals) | **No** — use `logic [N:0]` with explicit width |
| `byte`, `shortint`, `int`, `longint`, `integer` (parameters) | **Yes** |
| `time`, `realtime` | **Never supported** |
| `real`, `shortreal` (RTL signals) | **No** |
| `real`, `shortreal` (parameters) | **Yes** |
| `void` | **No** |
| `chandle` | **No** |
| `class` | **No** |
| `string` (RTL signals) | **No** |
| `string` (parameters) | **Yes** |
| `event` | **No** |
| `typedef` | **Yes** — for any supported type in context |
| Forward `typedef` | **Yes** |
| `enum` | **Yes** |
| Enum type methods | **Yes** |
| `parameter`, `localparam` | **Yes** |
| `specparam` | **No** |
| `const` | **No** |
| Package/compilation-unit scoping | **Yes** — see `sv_packages.md` |
| Size/signedness casts | **Yes** |
| `$cast` dynamic cast | **No** |
| Bit-stream casts | **No** |
| Type operator (`type(expr)`) | **TBD** |
| Parameterised data types | **No** |
