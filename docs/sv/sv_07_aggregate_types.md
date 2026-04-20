# SystemVerilog Section 7 — Aggregate Data Types: Support in This Compiler

This document describes how each feature of IEEE Std 1800-2023 §7 (Aggregate data
types) is handled by this compiler. The compiler targets **synchronous RTL only**:
it accepts synthesisable, clocked Verilog as input and lowers it into the
`Module` IR.

All section references below are to IEEE Std 1800-2023.

---

## 7.2 Structures

### 7.2.1 Packed structures

**Not supported (planned).** A packed structure (`struct packed { ... }`) is a
mechanism for subdividing a contiguous bit vector into named fields. Although packed
structures are synthesisable, the compiler does not yet support them. Encountering
a `struct` type in a signal or port declaration throws a compile error.

The planned IR representation is: a packed struct will be stored as a
`Type` with `kind = Integer` (total width = sum of field widths, MSB-first
per spec §7.2.1) plus a new metadata variant that records field names and their
bit-slice offsets. Member access (`sig.field`) will lower into a `SLICE` DFG node
using the precomputed offset. The struct is therefore treated as a flat bit vector
at the DFG level — identical to how single-dimension packed arrays are handled
today.

### 7.2.2 Unpacked structures

**Not supported (planned).** Unpacked structures (`struct { ... }` without
`packed`) have implementation-defined member layout and can contain any data type.
Support is planned alongside packed struct support.

### 7.2.3 Assigning to structures

**Not supported** while struct types are unsupported. Whole-struct assignment and
member-by-member assignment will become valid once struct support is implemented.
The `'{...}` assignment-pattern literal syntax (§5.10) is accepted by the parser
today; it will drive struct assignments once type support is in place.

---

## 7.3 Unions

### 7.3.1 Packed unions

**Not supported (planned).** A packed union (`union packed { ... }`) is a single
bit vector that can be interpreted as different member types. Like packed structs,
packed unions are synthesisable. Support is planned.

### 7.3.2 Soft packed unions

**Not supported (planned).** `union soft packed` unions, whose members need not be
the same size (LSB-justified, minimum width), are also planned.

### 7.3.3 Tagged unions

**Not supported.** Tagged unions (`union tagged { ... }`) carry an explicit
run-time tag field and require dynamic type-checking semantics. They have no
hardware-synthesisable counterpart in this compiler's model and will not be
supported.

---

## 7.4 Packed and unpacked arrays

### 7.4.1 Packed arrays (single and multi-dimensional)

**Supported.** A packed array is a contiguous bit vector where dimensions are
declared *before* the signal name:

```systemverilog
logic [7:0]       byte_sig;          // 1-D packed: 8 bits
logic [3:0][7:0]  wide_sig;          // 2-D packed: 32 bits
logic signed [15:0] s_sig;           // signed 1-D packed
```

**IR mapping:** stored as a single `Type` with `kind = Integer`:

| Field | Value |
|---|---|
| `width` | Product of all packed dimension sizes |
| `packed_dims` | `std::vector<ResolvedDimension>` — one entry per dimension, outermost first |
| `is_signed` | Per the `signed`/`unsigned` qualifier |

Examples:

```
logic [7:0]        → width=8,  packed_dims=[{7,0}]
logic [3:0][7:0]   → width=32, packed_dims=[{3,0},{7,0}]
logic signed [15:0]→ width=16, packed_dims=[{15,0}], is_signed=true
```

A 1-D packed vector (the common case) has exactly one entry in `packed_dims` and
is indistinguishable in the IR from a plain `logic [N:0]` declaration.

Only `bit`, `logic`, and `reg` element types are permitted for packed arrays (per
§7.4.1). All are treated identically in the IR (2-state; the 4-state distinction is
erased per §6.3 of the data-types documentation).

Packed arrays with integer types that have predefined widths (`byte`, `int`, etc.)
are **not supported** as RTL signal types (consistent with §6.11 of the data-types
documentation).

### 7.4.2 Unpacked fixed-size arrays

**Supported.** A fixed-size unpacked array has its dimension(s) declared *after*
the signal name:

```systemverilog
logic [7:0] arr [3:0];    // 4 elements of 8-bit logic
logic       flags [7:0];  // 8 single-bit elements
```

**IR mapping — signal declaration:**

The aggregate signal is registered in `Module::inputs`, `::outputs`, or
`::signals` with `unpacked_dims` set on its `Type`. For example,
`logic [7:0] arr [3:0]` produces:

```
Type { width=8, packed_dims=[{7,0}], unpacked_dims=[{3,0}] }
```

**IR mapping — DFG nodes:**

Each element of an unpacked array becomes a **separate, individually named DFG
node**. The node name is the signal base name concatenated with the index suffix in
`[i]` form. For `arr [3:0]` the four nodes are:

```
arr[0]  arr[1]  arr[2]  arr[3]
```

Each element node carries the element type (no `unpacked_dims`):
`Type { width=8, packed_dims=[{7,0}] }`.

There is **no limit** on the number of array elements.

**Reading an element with a constant index** resolves directly to the corresponding
named node.

**Reading an element with a variable (runtime) index** builds a MUX tree over all
element nodes: `MUX(sel==0, arr[0], MUX(sel==1, arr[1], ...))`.

**Writing an element** in a procedural block generates an assignment to the
corresponding element node. Variable-index writes are not supported (the index must
be statically known at elaboration time).

### 7.4.3 Memories

A *memory* is a 1-D unpacked array whose element type is a packed vector
(`logic [N:0]` or `bit [N:0]`):

```systemverilog
logic [7:0] mem [0:255];   // 256 × 8-bit words
```

**Supported.** Memories follow exactly the same IR mapping as unpacked fixed-size
arrays (§7.4.2): 256 individual DFG nodes named `mem[0]` through `mem[255]`.
They are the standard way to represent register files or lookup tables in RTL.

### 7.4.4 Multidimensional arrays

**Packed multi-dimensional arrays:** fully supported. Dimensions are recorded in
`packed_dims` outermost-first; total `width` is their product. See §7.4.1.

**Unpacked multi-dimensional arrays:** supported. Each element at the full leaf
index path becomes an individual DFG node. For example:

```systemverilog
logic [7:0] grid [1:0][3:0];   // 2 rows × 4 columns of 8-bit elements
```

Produces 8 DFG nodes: `grid[1][0]`, `grid[1][1]`, `grid[1][2]`, `grid[1][3]`,
`grid[0][0]`, …, `grid[0][3]`. The aggregate signal in
`Module::signals` carries
`unpacked_dims=[{1,0},{3,0}]`.

Mixed packed/unpacked multidimensional arrays are also supported:

```systemverilog
logic [3:0][7:0] wide_arr [3:0];
// 4 elements, each a 32-bit packed 2-D vector
// DFG nodes: wide_arr[0] … wide_arr[3], each width=32
```

### 7.4.5 Indexing and slicing of arrays

**Packed arrays — indexing and part-select:**

Element selection and part-selects on packed arrays resolve at elaboration time
to `SLICE` DFG nodes:

```systemverilog
logic [31:0] word;
logic [7:0]  byte_out;

byte_out = word[15:8];   // SLICE(word, high=15, low=8)
byte_out = word[7:0];    // SLICE(word, high=7,  low=0)
```

For multi-dimensional packed arrays, indexing into an outer dimension yields a
SLICE spanning the sub-vector's full width:

```systemverilog
logic [3:0][7:0] wide;
logic [7:0] elem;
elem = wide[2];    // SLICE(wide, high=23, low=16)  — third 8-bit chunk
```

Slicing with a variable low-offset (`+:` / `-:` part-selects) is **supported**:

```systemverilog
byte_out = word[idx +: 8];  // SLICE with runtime offset
```

The *width* of a `+:`/`-:` part-select must be a constant; the *position* may be
a runtime expression.

**Unpacked arrays — indexing:**

Element selection on an unpacked array resolves to the named element node (constant
index) or a MUX tree (variable index). Slicing an unpacked array (selecting a
contiguous sub-array) is **not yet supported**.

### 7.4.6 Operations on arrays

| Operation | Packed arrays | Unpacked fixed-size arrays |
|---|---|---|
| Whole-array read/write (`A = B`) | **Yes** — treated as a vector | **Yes** — element-by-element assignment |
| Element read (`A[i]`) | **Yes** — SLICE | **Yes** — named node or MUX tree |
| Element write (`A[i] = x`) | **Yes** — CONCAT/SLICE | **Yes** — constant index only |
| Part-select read (`A[h:l]`) | **Yes** — SLICE | **No** |
| Constant-width variable-offset read (`A[i +: k]`) | **Yes** — SLICE | **No** |
| Arithmetic/logical operators (`A + 3`) | **Yes** — vector semantics | **No** |
| Equality (`A == B`) | **Yes** | **No** |
| Signed interpretation | **Yes** — `signed` qualifier | **No** |

---

## 7.5 Dynamic arrays

**Not supported.** Dynamic arrays have variable run-time size (`arr[]` with
`new[]` constructor) and no static hardware representation. They are not
synthesisable.

---

## 7.6 Array assignments

**Supported for fixed-size unpacked arrays.** Whole-array assignment
(`arr_a = arr_b`) where both arrays have the same element type and the same number
of elements is valid in RTL. The compiler generates element-by-element assignments
in declaration order. The arrays need not have the same index range, only the same
element count.

Packed-array assignment follows vector assignment rules (§10.7 of the spec):
source and target widths need not match; truncation or zero/sign-extension is
applied.

---

## 7.7 Arrays as arguments to subroutines

Not applicable in the synthesisable RTL subset. Functions are inlined during
elaboration; there is no run-time argument-passing mechanism. Array arguments to
inlined functions follow the same rules as array assignments (§7.6).

---

## 7.8 Associative arrays

**Not supported.** Associative arrays use dynamic, sparse storage indexed by
arbitrary types. They are not synthesisable.

---

## 7.9 Associative array methods

**Not supported.** `num()`, `delete()`, `exists()`, `first()`, `last()`,
`next()`, `prev()` etc. are not applicable.

---

## 7.10 Queues

**Not supported.** Queues (`arr[$]`) support dynamic insertion and removal and are
not synthesisable.

---

## 7.11 Array querying functions

**Not yet implemented.** The system functions `$left`, `$right`, `$low`, `$high`,
`$increment`, `$size`, `$dimensions`, and `$unpacked_dimensions` (§20.7) can return
static information about array bounds and are legal in constant expressions and
generate-construct loop bounds. Support is planned; they currently throw a compile
error if used.

---

## 7.12 Array manipulation methods

**Not supported.** The locator methods (`find`, `find_index`, `min`, `max`,
`unique`, etc.), ordering methods (`sort`, `rsort`, `reverse`, `shuffle`), and
reduction methods (`sum`, `product`, `and`, `or`, `xor`) operate on unpacked arrays
at run time and are not synthesisable.

---

## Summary table

| §7 feature | Support |
|---|---|
| Packed structures (`struct packed`) | **No** — planned |
| Unpacked structures (`struct`) | **No** — planned |
| Packed unions (`union packed`) | **No** — planned |
| Soft packed unions (`union soft packed`) | **No** — planned |
| Tagged unions (`union tagged`) | **No** — not planned |
| 1-D packed arrays (`logic [N:0]`) | **Yes** — `Type` with `packed_dims` |
| Multi-dimensional packed arrays | **Yes** — multiple entries in `packed_dims` |
| Fixed-size unpacked arrays | **Yes** — expanded to per-element DFG nodes |
| Multidimensional unpacked arrays | **Yes** — expanded to per-element DFG nodes |
| Memories (`logic [N:0] mem [M:0]`) | **Yes** — same as unpacked arrays |
| Packed element/part-select (constant) | **Yes** — `SLICE` DFG node |
| Packed variable-offset part-select (`+:`) | **Yes** — `SLICE` with runtime offset |
| Unpacked element select (constant index) | **Yes** — named node |
| Unpacked element select (variable index) | **Yes** — MUX tree over elements |
| Unpacked variable-index write | **No** |
| Unpacked sub-array slice | **No** |
| Whole-array assignment (unpacked) | **Yes** — element-by-element |
| Dynamic arrays | **No** |
| Associative arrays | **No** |
| Queues | **No** |
| Array querying functions (`$size`, etc.) | **No** — planned |
| Array manipulation methods | **No** |
