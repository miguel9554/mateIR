# SystemVerilog Generate Constructs (IEEE 1800-2023, §27)

> **Implementation note**: We use slang for parsing only (AST), not elaboration.
> Generate constructs appear as raw syntax nodes and **we must evaluate them ourselves**
> during our elaboration pass. This document is a complete reference for that work.

---

## 27.1 Topics covered

- Loop generate constructs
- Conditional generate constructs
- External names in unnamed generate constructs

---

## 27.2 Overview

A **generate block** is a collection of one or more module items. Generate constructs
either conditionally or multiply instantiate generate blocks into a module.

### What a generate block may NOT contain
- Port declarations
- `specify` blocks
- `specparam` declarations

Parameters declared inside a generate block are treated as **`localparam`** (see §6.20.4).
All other module items are allowed, including nested generate constructs.

### What generate constructs enable
- Parameter values affecting design structure
- Concise description of modules with repetitive structure
- Recursive module instantiation

### Elaboration result
Elaborating a generate construct produces **zero or more instances** of a generate block.
Each instance:
- Creates a **new level of hierarchy** and a **new scope**
- Brings objects, behavioral constructs, and module instances into existence
- Behaves like a module instantiation, except that declarations from the **enclosing scope
  can be referenced directly** (§23.9)
- If the block is named, its contents can be referenced **hierarchically** (§23.6)

### `generate` / `endgenerate` keywords
These delimit a **generate region** — a textual span where generate constructs may appear.
- Their use is **optional** — no semantic difference whether present or not
- Generate regions **do not nest**
- May only appear **directly within a module**
- If `generate` is used, it must be matched by `endgenerate`

A parser may recognise the generate region to produce better error messages, but tools
must accept generate constructs outside a generate region as well.

---

## 27.3 Syntax (Syntax 27-1)

```
generate_region ::=
    generate { generate_item } endgenerate

loop_generate_construct ::=
    for ( genvar_initialization ; genvar_expression ; genvar_iteration )
        generate_block

genvar_initialization ::=
    [ genvar ] genvar_identifier = constant_expression

genvar_iteration ::=
    genvar_identifier assignment_operator genvar_expression
  | inc_or_dec_operator genvar_identifier
  | genvar_identifier inc_or_dec_operator

conditional_generate_construct ::=
    if_generate_construct
  | case_generate_construct

if_generate_construct ::=
    if ( constant_expression ) generate_block [ else generate_block ]

case_generate_construct ::=
    case ( constant_expression ) { case_generate_item } endcase

case_generate_item ::=
    constant_expression { , constant_expression } : generate_block
  | default [ : ] generate_block

generate_block ::=
    generate_item
  | [ generate_block_identifier : ] begin [ : generate_block_identifier ]
        { generate_item }
    end [ : generate_block_identifier ]
```

A `generate_item` (in a module context) may be any `module_or_generate_item`:
- Gate instantiation
- Module instantiation
- Continuous assignment (`assign`)
- Net/variable declarations
- `always`/`initial`/`final` constructs
- Parameter/localparam declarations
- `genvar` declaration
- Other nested `loop_generate_construct` or `conditional_generate_construct`

---

## 27.4 Loop Generate Constructs

A loop generate construct instantiates a generate block **zero or more times** using
syntax similar to a `for` loop statement.

### genvar

- The loop index must be declared as a **`genvar`** prior to use in the loop.
- `genvar` is an **elaboration-time integer** — it does NOT exist at simulation time.
- A `genvar` shall **not be referenced anywhere other than** in a loop generate scheme.
- Both the initialization and iteration assignments must assign to the **same `genvar`**.
- The initialization assignment must **not reference** the loop index on the RHS.
- It is an error if any bit of the `genvar` is `x` or `z` during evaluation.
- It is an error if the loop does **not terminate**.
- It is an error if a `genvar` value is **repeated** during evaluation (i.e., the loop
  must visit each index value at most once).

### Implicit `localparam` inside the loop body

Within the generate block of a loop construct there is an **implicit `localparam`**:
- Same name and type as the loop index `genvar`
- Value in each block instance = value of the `genvar` at the time that instance was elaborated
- Can be used anywhere inside the block where a normal integer parameter can be used
- Can be referenced with a hierarchical name from outside the block

Because this implicit `localparam` has the **same name** as the `genvar`, any reference
to that name inside the loop body resolves to the `localparam`, not the `genvar`.
**Consequence**: it is impossible to have two nested loop generate constructs using the
same `genvar` name, because the outer `genvar` would be shadowed by the inner `localparam`.

### Named vs. unnamed generate blocks in a loop

A generate block in a loop may consist of a single item without `begin`-`end`; it is
still a generate block and still creates a separate scope and a new level of hierarchy
when instantiated.

**Named block** (e.g., `for (...) begin : bitnum ... end`):
- Declares an **array of generate block instances** indexed by the `genvar` values used
  during elaboration.
- The index values are the actual `genvar` values — the array may be **sparse** (values
  need not form a contiguous integer range).
- The array is considered declared even if the loop results in **zero instances**.
- Hierarchical path: `module_name.block_name[genvar_value].member`
- Error if the block instance array name conflicts with any other declaration in the
  same scope, including other generate block instance arrays.

**Unnamed block**:
- Declarations within cannot be referenced using hierarchical names from outside the
  hierarchy instantiated by the generate block itself.

### Error conditions summary for loop generates

| Condition | Result |
|---|---|
| `genvar` has `x` or `z` bits | error |
| Loop does not terminate | error |
| `genvar` value repeated within loop | error |
| Named block name conflicts with any declaration in same scope | error |
| Same named block identifier used by two separate loops (same scope) | error |

### Examples

**Example 1** — Legal and illegal loop generates:

```systemverilog
module mod_a;
    genvar i;
    // generate/endgenerate keywords are not required
    for (i=0; i<5; i=i+1) begin:a
        for (i=0; i<5; i=i+1) begin:b  // ERROR: "i" used as loop index for two nested loops
            ...
        end
    end
endmodule

module mod_b;
    genvar i;
    logic a;
    for (i=1; i<0; i=i+1) begin:a   // ERROR: block name "a" conflicts with variable "a"
        ...
    end
endmodule

module mod_c;
    genvar i;
    for (i=1; i<5;  i=i+1) begin:a ... end
    for (i=10; i<15; i=i+1) begin:a  // ERROR: "a" conflicts with previous loop block "a"
        ...
    end
endmodule
```

**Example 2** — Gray-to-binary converter using continuous assignments:

```systemverilog
module gray2bin1 (bin, gray);
    parameter SIZE = 8;
    output [SIZE-1:0] bin;
    input  [SIZE-1:0] gray;

    genvar i;
    generate
        for (i=0; i<SIZE; i=i+1) begin:bitnum
            assign bin[i] = ^gray[SIZE-1:i];
            // "i" here refers to the implicit localparam, not the genvar
        end
    endgenerate
endmodule
```

**Example 3** — Ripple adder, net declared outside loop:

```systemverilog
module addergen1 (co, sum, a, b, ci);
    parameter SIZE = 4;
    output [SIZE-1:0] sum;
    output            co;
    input  [SIZE-1:0] a, b;
    input             ci;
    wire   [SIZE  :0] c;
    wire   [SIZE-1:0] t [1:3];
    genvar            i;

    assign c[0] = ci;

    for (i=0; i<SIZE; i=i+1) begin:bitnum
        xor g1 ( t[1][i], a[i],    b[i]);
        xor g2 ( sum[i],  t[1][i], c[i]);
        and g3 ( t[2][i], a[i],    b[i]);
        and g4 ( t[3][i], t[1][i], c[i]);
        or  g5 ( c[i+1],  t[2][i], t[3][i]);
    end
    // Hierarchical gate names: bitnum[0].g1 ... bitnum[3].g5

    assign co = c[SIZE];
endmodule
```

**Example 4** — Ripple adder, net declared inside loop:

```systemverilog
    for (i=0; i<SIZE; i=i+1) begin:bitnum
        wire t1, t2, t3;   // one t1/t2/t3 per iteration
        xor g1 ( t1,     a[i],  b[i]);
        xor g2 ( sum[i], t1,    c[i]);
        and g3 ( t2,     a[i],  b[i]);
        and g4 ( t3,     t1,    c[i]);
        or  g5 ( c[i+1], t2,    t3);
    end
    // Hierarchical net names: bitnum[0].t1, bitnum[1].t1, ...
```

**Example 5** — Multilevel (nested) generate loops:

```systemverilog
parameter SIZE = 2;
genvar i, j, k, m;
generate
    for (i=0; i<SIZE; i=i+1) begin:B1          // scope B1[i]
        M1 N1();                                // B1[i].N1
        for (j=0; j<SIZE; j=j+1) begin:B2      // scope B1[i].B2[j]
            M2 N2();                            // B1[i].B2[j].N2
            for (k=0; k<SIZE; k=k+1) begin:B3  // scope B1[i].B2[j].B3[k]
                M3 N3();                        // B1[i].B2[j].B3[k].N3
            end
        end
        if (i>0) begin:B4
            for (m=0; m<SIZE; m=m+1) begin:B5  // scope B1[i].B4.B5[m]
                M4 N4();                        // B1[i].B4.B5[m].N4
            end
        end
    end
endgenerate

// Example hierarchical instance names:
// B1[0].N1        B1[1].N1
// B1[0].B2[0].N2  B1[0].B2[1].N2
// B1[0].B2[0].B3[0].N3   B1[0].B2[0].B3[1].N3
// B1[0].B2[1].B3[0].N3
// B1[1].B4.B5[0].N4   B1[1].B4.B5[1].N4
```

---

## 27.5 Conditional Generate Constructs

Select **at most one** generate block from a set of alternatives, based on
**constant expressions** evaluated at elaboration time. The selected block, if any,
is instantiated into the model.

### if-generate

```systemverilog
if (constant_expression) generate_block
                    [ else generate_block ]
```

- `else` binds to the **nearest `if`**, same as in procedural code.
- To attach an `else` to an outer `if`, insert an explicit `else ;` (null generate
  block) after the inner `if`.

### case-generate

```systemverilog
case (constant_expression)
    val1 [, val2, ...] : generate_block
    ...
    default            : generate_block
endcase
```

- Case items are **constant expressions**.
- `default` is optional; if no case matches and there is no default, no block is instantiated.
- Parenthesised tuple expressions like `(MEM_SIZE, MEM_WIDTH)` are valid case
  expressions (they are constant expressions formed with the concatenation/tuple rules).

### Naming rules for conditional generate blocks

- At most one alternative is instantiated, so **multiple alternatives within the same
  `if`/`case` construct may share the same block name** — only one will ever be instantiated.
- Named generate blocks must **not** have the same name as generate blocks in any
  **other** conditional or loop generate construct in the same scope, even if those
  blocks are never selected.
- Named generate blocks must **not** have the same name as any other declaration in the
  same scope, even if the block is never selected for instantiation.
- If the selected block is **named**: its name declares a generate block instance and
  is the name for the scope it creates. Normal hierarchical naming rules apply.
- If the selected block is **unnamed**: it still creates a scope, but declarations
  within it cannot be referenced hierarchically from outside.

### Direct nesting (flattening of if-else-if chains)

If a conditional generate block:
1. Consists of **exactly one item** that is itself a conditional generate construct, AND
2. Is **not** surrounded by `begin`-`end`

…then it is considered **directly nested**. The directly nested inner construct is
treated as if its generate blocks belong to the outer construct. This means:

- The inner construct's generate blocks **can** share the same name as the outer
  construct's generate blocks (because only one will ever be instantiated).
- They **cannot** have the same name as any declaration in the scope enclosing the
  outer construct (including other generate blocks in that scope).
- This allows `if-else-if` chains of arbitrary depth without creating unnecessary
  hierarchy levels.
- Direct nesting applies **only** to conditional-in-conditional. It does **not** apply
  to loop generate constructs.

```systemverilog
// Direct nesting: all begin:u1 blocks are alternatives of the same flat construct
if (p == 1)
    if (q == 0)
        begin:u1 and g1(a,b,c); end        // test.u1.g1
    else if (q == 2)
        begin:u1 or  g1(a,b,c); end        // test.u1.g1
    else ;                                  // null block to push else to outer if
else if (p == 2)
    case (q)
        0,1,2:   begin:u1 xor  g1(a,b,c); end  // test.u1.g1
        default: begin:u1 xnor g1(a,b,c); end   // test.u1.g1
    endcase
// All six begin:u1 blocks share the same name legally.
// The hierarchical name of g1 is always test.u1.g1 regardless of which is chosen.
```

### Recursive module instantiation via conditional generates

Conditional (and loop) generate constructs allow a module to contain an instantiation
of itself. With appropriate parameter conditions the recursion terminates at elaboration
time, producing a legitimate model. A module containing a self-instantiation is never
a top-level module.

**Example** — Parameterised multiplier choosing implementation based on width:

```systemverilog
module multiplier(a, b, product);
    parameter a_width = 8, b_width = 8;
    localparam product_width = a_width + b_width;
    input  [a_width-1:0]    a;
    input  [b_width-1:0]    b;
    output [product_width-1:0] product;

    generate
        if ((a_width < 8) || (b_width < 8)) begin:mult
            CLA_multiplier #(a_width, b_width) u1(a, b, product);
        end else begin:mult
            WALLACE_multiplier #(a_width, b_width) u1(a, b, product);
        end
    endgenerate
    // Hierarchical instance name is always mult.u1
endmodule
```

**Example** — case-generate choosing adder implementation:

```systemverilog
generate
    case (WIDTH)
        1: begin:adder adder_1bit #(WIDTH) x1(co,sum,a,b,ci); end
        2: begin:adder adder_2bit #(WIDTH) x1(co,sum,a,b,ci); end
        default: begin:adder adder_cla #(WIDTH) x1(co,sum,a,b,ci); end
    endcase
endgenerate
// Hierarchical instance name is adder.x1
```

**Example** — case-generate on a tuple expression (DIMM memory):

```systemverilog
module dimm(addr, ba, rasx, casx, csx, wex, cke, clk, dqm, data, dev_id);
    parameter [31:0] MEM_WIDTH = 16, MEM_SIZE = 8;
    genvar i;

    case ({MEM_SIZE, MEM_WIDTH})
        {32'd8, 32'd16}: begin:memory       // 8Meg x 16 bits
            for (i=0; i<4; i=i+1) begin:word16
                sms_08b216t0 p(.clk(clk), ...);
            end
            // instance names: memory.word16[3].p ... memory.word16[0].p
        end
        {32'd16, 32'd8}: begin:memory       // 16Meg x 8 bits
            for (i=0; i<8; i=i+1) begin:word8
                sms_16b208t0 p(.clk(clk), ...);
            end
        end
        // Other memory cases ...
    endcase
endmodule
```

---

## 27.6 External Names for Unnamed Generate Blocks

Although unnamed generate blocks have no user-given name, external interfaces
(hierarchical bind, debug, etc.) need a stable name for them. The rule:

### Numbering algorithm

1. Each generate **construct** (not block) in a given scope is assigned a sequential
   integer, starting at **1**, incrementing by **1** for each subsequent generate
   construct in **textual order** within that scope.
2. Every generate construct is counted even if it contains **no unnamed blocks**.
3. All unnamed generate blocks within construct number `n` receive the name
   **`genblk<n>`**.
4. If `genblk<n>` **conflicts** with an explicitly declared name in the same scope,
   prepend leading zeros until the name is unique (e.g., `genblk02`, `genblk002`, …).
5. The numbering restarts at 1 within each nested scope.

### Example

```systemverilog
module top;
    parameter genblk2 = 0;   // conflicts with genblk2
    genvar i;

    // Construct #1: unnamed blocks → genblk1
    if (genblk2) logic a; else logic b;
    // top.genblk1.a  or  top.genblk1.b

    // Construct #2: "genblk2" conflicts with parameter → genblk02
    if (genblk2) logic a; else logic b;
    // top.genblk02.a  or  top.genblk02.b

    // Construct #3: explicitly named "g1" (still consumes slot #3)
    for (i=0; i<1; i=i+1) begin:g1
        // nested scope — restart counter at 1
        // Construct #1 inside g1:
        if (1) logic a;     // top.g1[0].genblk1.a
    end

    // Construct #4: unnamed blocks → genblk4
    // (slot #3 was consumed by the named "g1" construct above)
    for (i=0; i<1; i=i+1)
        // nested scope — restart counter at 1
        // Construct #1 inside genblk4:
        if (1) logic a;     // top.genblk4[0].genblk1.a

    // Construct #5: unnamed block → genblk5
    if (1) logic a;         // top.genblk5.a
endmodule
```

---

## Implementation Requirements

Since we use slang for AST only (not elaboration), **we must implement generate
elaboration ourselves** during our elaboration pass. The following describes
everything that must be implemented.

### Constant expression evaluator

All generate scheme expressions (loop bounds, conditions, case selectors, case items)
are **constant expressions**. We must be able to evaluate expressions composed of:
- Integer literals (including sized literals like `32'd8`)
- Named parameters and localparams (resolved from the enclosing scope)
- Arithmetic, relational, logical, bitwise, reduction, shift, conditional operators
- Concatenation `{a, b}` (used in tuple case expressions)
- Hierarchical parameter references for parameterised submodule context

This evaluator must be available before any generate block is instantiated because
loop bounds and conditions must be resolved to decide which blocks exist.

### genvar management

- `genvar` declarations appear as module items (inside or outside `generate` regions).
- They are elaboration-time only. Do not create DFG nodes or signals for them.
- Track `genvar` current value in an elaboration-time symbol table.
- When entering a loop body, bind the implicit `localparam` (same name, same value)
  in the block's scope so that references inside the block resolve to the frozen value.

### Scope and hierarchy

Each generate block instance creates a new scope (even without `begin`-`end`).
- For a **loop** instance at index `v` of named block `blk`: path segment is `blk[v]`
- For a **conditional** named block `blk`: path segment is `blk`
- For unnamed blocks: path segment is the auto-assigned name (see §27.6)

The scope must:
- Allow the block body to reference names from the enclosing scope directly
- Give the block's own declarations precedence over outer scope names
- Support hierarchical path construction for submodule instances and signals

### Loop elaboration algorithm

```
function elaborate_loop(for_init, for_cond, for_iter, body, scope):
    evaluate for_init → set genvar to initial value
    while eval(for_cond) is true:
        check genvar has no x/z bits
        check genvar value not previously seen in this loop → error if duplicate
        create new block instance scope with implicit localparam = current genvar value
        elaborate body in that scope
        evaluate for_iter → update genvar
        record genvar value as seen
    // zero iterations is legal
```

### Conditional elaboration algorithm

**if-generate**:
```
evaluate constant_expression
if true:  elaborate if-branch generate_block
else:     elaborate else-branch generate_block (if present)
```

**case-generate**:
```
evaluate case selector expression
for each case_generate_item in order:
    if selector matches any item constant expression:
        elaborate corresponding generate_block
        stop
if no match and default exists:
    elaborate default generate_block
// no match and no default: zero blocks instantiated (legal)
```

### Direct nesting detection

When elaborating a conditional generate block that:
1. Contains exactly one item AND
2. That item is a conditional generate construct (not a loop) AND
3. The block has no `begin`-`end`

→ do NOT create a new scope level; treat the inner construct's blocks as belonging
to the outer construct's scope. This is critical for correct `if-else-if` flattening.

### Auto-naming of unnamed blocks (§27.6)

Maintain a per-scope counter (starting at 1) that increments for **every** generate
construct encountered in textual order, whether or not it contains unnamed blocks.
For each unnamed block in construct `n`, assign name `genblk<n>`. If that name
conflicts with any existing declaration in the scope, prepend zeros until unique.
Nested scopes restart the counter at 1.

### What to produce from a generate block instance

Treat the elaborated contents exactly as if they had appeared directly at the module
level, except:
- All signal/instance names are qualified with the generate block instance path.
- `localparam` values frozen at elaboration time are available for constant-folding
  within the block (e.g., used as array indices, port widths, parameter overrides).
- Nested generate constructs inside a block are recursively elaborated.

### Error conditions to enforce

| Condition | Error |
|---|---|
| `genvar` bit is `x` or `z` | yes |
| Loop does not terminate | yes |
| `genvar` value repeated within a single loop | yes |
| Named generate block array name conflicts with any other declaration in scope | yes |
| Two generate constructs in same scope share a named block identifier (unless direct nesting allows it) | yes |
| Named block shares name with any non-generate declaration in scope (even if never instantiated) | yes |
