# SystemVerilog Packages — Scoping Specification

This document specifies how package declarations and imports work in SystemVerilog,
scoped to the constructs supported by this simulator. The following constructs are
**not supported** and are out of scope: `program`, `checker`, `class`. `interface`
is in scope but not yet implemented.

All rules derive from IEEE Std 1800-2023, Clause 26 and §3.12.1.

---

## 1. Package declarations

A package is an explicitly named scope at the outermost level of the source text,
at the same level as top-level modules.

**Package names are globally unique** across all compilation units (§3.13). Once a
name is used to declare a package in any compilation unit, no other compilation unit
may declare another package with the same name. This is enforced at the global
package namespace level, not per-file.

**A package must be compiled before any scope that imports from it.** Consequently,
**circular package imports are forbidden**: if package `p` imports from package `q`,
then `q` cannot import from `p` (directly or transitively), since `p` would have to
be compiled before `q` and `q` before `p` simultaneously.

Two important restrictions on references inside a package body:
- Items within a package shall **not** have hierarchical references to identifiers
  outside the package. Only identifiers declared within the package itself, or made
  visible by importing another package, may be referenced.
- A package shall **not** refer to items defined in the compilation-unit scope.

See Section 6 for the full table of allowable items.

---

## 2. Where imports are allowed

### 2.1 Compilation-unit scope

Package imports may appear at the top level of a compilation unit, outside all
other declarations. This is legal because the compilation-unit scope can contain
any item that can be defined within a package, including `data_declaration`, of
which `package_import_declaration` is a form.

```systemverilog
import mypkg::my_type;  // at the top level, outside any module
import utils::*;

module foo;
  my_type x;  // visible here
endmodule
```

Identifiers made visible by imports at the compilation-unit scope are available
to all modules and other constructs within that compilation unit.

Note: you cannot import **from** the compilation-unit scope — it has no name.
You can only import **into** it.

### 2.2 Module headers

Package imports may appear in the module header, between the module name and the
`#(` parameter list or port list. This is true for both ANSI and non-ANSI forms.

```
module_nonansi_header ::= ... module_identifier { package_import_declaration } [ parameter_port_list ] list_of_ports ;
module_ansi_header    ::= ... module_identifier { package_import_declaration } [ parameter_port_list ] [ list_of_port_declarations ] ;
```

For ANSI headers, a `package_import_declaration` must be followed by a parameter
list, a port list, or both.

```systemverilog
module M import A::instruction_t, B::*;
  #(parameter int WIDTH = 32)
  (input instruction_t a,   // instruction_t is visible here
   output logic [WIDTH-1:0] result);
  ...
endmodule
```

**This is the only way to make package identifiers visible in parameter and port
declarations.** Body-level imports are processed after the port list and cannot
affect it.

### 2.3 Module bodies (and generate blocks)

Package imports may appear anywhere inside a module body where a `data_declaration`
is allowed, including inside named and unnamed generate blocks.

```systemverilog
module M;
  import mypkg::*;   // visible from this point onward in this scope
  ...
endmodule
```

Inside a generate block, an import is scoped to that generate block and outward
per the normal lexical scope search rules (see Section 3).

### 2.4 Package bodies

A package may import from other packages. By default, imported names are **not
re-exported** — they are not visible to scopes that subsequently import the
importing package. Re-export requires an explicit `export` declaration (see
Section 5).

```systemverilog
package p2;
  import p1::some_type;  // visible within p2 only, unless exported
  ...
endpackage
```

### 2.5 Interface headers and bodies

The same rules as modules apply: imports are allowed in the interface header
(making them visible in parameter and port declarations) and anywhere in the
interface body.

**Not yet implemented.**

### 2.6 Constructs where imports are NOT allowed

| Construct | Status |
|-----------|--------|
| `program` | Not supported by this simulator |
| `checker` | Not supported by this simulator |
| `class` body | Not supported; also explicitly illegal in the spec (§26.3) |

---

## 3. Scoping rules

### 3.1 Explicit import

```systemverilog
import pkg::name;
```

- Makes exactly the named identifier directly visible in the current scope.
- Does **not** import other identifiers from the same package.
- Illegal if the same identifier is already declared locally in the same scope,
  already explicitly imported from a different package, **or already pulled into
  the scope via wildcard resolution**.
- Importing the same identifier from the same package multiple times is allowed.
- Enumeration type imports do **not** automatically import the type's literals;
  those must be imported explicitly or accessed via `pkg::LITERAL`.

### 3.2 Wildcard import

```systemverilog
import pkg::*;
```

- Makes all declarations in `pkg` **potentially locally visible** — they are not
  immediately injected into the scope.
- An identifier from a wildcard import only becomes **locally visible** (i.e.,
  actually imported) when a reference is resolved and no other locally visible
  identifier matches. At that moment the identifier is imported into the scope and
  becomes a locally visible declaration.
- Once a wildcard import pulls an identifier into a scope, any later local
  declaration of the same identifier in that scope is **illegal**.
- If two wildcard imports from different packages within the same scope both define
  the same identifier and a reference resolves to that identifier, it is an
  **error**.

### 3.3 Identifier resolution algorithm

An identifier is **locally visible** at some point within a scope if:
- (a) it denotes a nested scope within the current scope, or
- (b) it is declared as an identifier prior to that point within the current scope, or
- (c) it is visible from an explicit import prior to that point within the current scope.

An identifier is **potentially locally visible** if there is a wildcard import of a
package before that point in the current scope and the package contains a declaration
of that identifier.

For a reference to identifier `x` at a given point in scope `S`:

1. Search the locally visible identifiers in `S` at the point of reference (conditions
   a, b, c above).

   Exception: for **function or task calls**, search all locally visible
   identifiers to the **end** of `S`, not just those before the reference point.

   Exception: for **function or task calls**, search all locally visible
   identifiers to the **end** of `S`, not just those before the reference point.

2. If not found, search the potentially locally visible identifiers from wildcard
   imports in `S` defined prior to the reference. If a match is found, that
   identifier is imported into `S` and the reference binds to it.

3. If still not found, repeat steps 1 and 2 in the next outer lexical scope.

4. Continue outward through each enclosing scope until the compilation-unit scope
   is reached (the final scope searched).

5. If no match is found, it is a compile error (for non-function/task references).

### 3.4 Textual ordering

Imports are **textually ordered**. A wildcard import only makes identifiers
potentially visible to references that appear **after** the import statement,
except for function/task calls which search the entire scope (see 3.3 step 1).

Example: given `import p::*` in an outer scope, a reference to `f()` in an inner
scope binds to `p::f` even if the import appears after the call site, because
function calls search to the end of scope. For non-call references, only imports
lexically preceding the reference are considered.

### 3.5 Import does not copy

An import makes an identifier **visible** in the importing scope; it does not copy
the declaration. Specifically:

- The imported identifier is **not** visible outside the importing scope via
  hierarchical reference into that scope.
- The imported identifier is **not** visible via interface port reference into that
  scope.
- There is only one copy of the declaration; it lives in the package.

### 3.6 Qualified access without import

Any package identifier can always be accessed with the `::` operator regardless of
imports:

```systemverilog
mypkg::my_type x;        // no import needed
y = mypkg::some_func();  // no import needed
```

Qualified access works in any scope, at any point, with no scoping restrictions.

---

## 4. Search order summary table

The following table shows how a reference to `c` resolves under different import
configurations, given `package p` defines `c` and `package q` defines `c`.

| Scope contains | `c` resolves to | Notes |
|---|---|---|
| Local declaration of `c` | local `c` | Import of same name from another pkg is illegal |
| `import p::c` only | `p::c` | Explicit import |
| `import p::*` only | `p::c` (if referenced) | Wildcard: only becomes visible on first reference |
| `import p::*` and `import q::*`, both define `c` | **Error** | Ambiguous wildcard |
| `import p::c` and `import q::c` | **Error** | Cannot explicitly import same name from two packages |
| Local declaration of `c` and `import p::c` | **Error** | Cannot explicitly import name already locally declared |
| `import p::*` and later local declaration of `c` after `c` has been referenced | **Error** | Wildcard-pulled name conflicts with later local decl |
| `import q::*`, `c` resolved via wildcard (pulling `q::c` in), then `import p::c` | **Error** | Explicit import illegal — `c` already locally visible via wildcard resolution |
| No declaration, no import | **Error** | Undefined identifier |

---

## 5. Package exports

By default, when package `p2` imports from package `p1`, those imported names are
invisible to any scope that subsequently imports `p2`. To make them visible, `p2`
must re-export them:

```systemverilog
package p2;
  import p1::x;
  export p1::x;    // x is now visible to importers of p2
  export p1::*;    // all actually-imported names from p1 are visible
  export *::*;     // all actually-imported names from all packages are visible
endpackage
```

**Export may precede its corresponding import.** A `package_export_declaration` is
allowed to appear before the `package_import_declaration` it refers to within the
same package body.

**`export pkg::name` counts as a reference.** An explicit export of the form
`export pkg::name` is treated as a reference to `name`, triggering its import into
the package following the same rules as a direct import. Consequently, a subsequent
local declaration of that name in the same package is **illegal**:

```systemverilog
package p6;
  import p1::*;
  export p1::x;   // counts as a reference; x is now imported
  int x;          // ERROR: x already locally visible from the export/import above
endpackage
```

Only names that were **actually imported** (referenced) are exported — wildcard
import candidates that were never referenced are not exported even with `export p1::*`.

An import of a name via an export chain is equivalent to importing the original
declaration; multiple export paths to the same underlying declaration do not
cause conflicts.

---

## 6. Allowable items inside a package

The grammar production `package_item` is the top-level item inside a package body.
It expands as follows. Each row shows the spec rule, whether it is allowed in this
simulator, and any further constraints.

### 7.1 `package_or_generate_item_declaration`

| Spec item | Simulator | Notes |
|---|---|---|
| `net_declaration` | **No** | No net support |
| `data_declaration` → variable decl | **No** | No variables inside packages |
| `data_declaration` → `type_declaration` (`typedef`) | **Yes** | Primary use case for packages |
| `data_declaration` → `package_import_declaration` | **Yes** | Needed for a package to import from another package |
| `data_declaration` → `nettype_declaration` | **No** | No net support |
| `task_declaration` | **Not yet** | Planned |
| `function_declaration` | **Not yet** | Planned |
| `checker_declaration` | **No** | Checker not supported |
| `dpi_import_export` | **No** | No DPI support |
| `extern_constraint_declaration` | **No** | Constraints not supported |
| `class_declaration` | **No** | Class not supported |
| `interface_class_declaration` | **No** | OOP-only construct (unrelated to `interface`); class not supported |
| `class_constructor_declaration` | **No** | Class not supported |
| `local_parameter_declaration` | **Yes** | |
| `parameter_declaration` | **Yes** | In package scope, `parameter` is always a synonym for `localparam`; cannot be overridden |
| `covergroup_declaration` | **No** | Coverage not supported |
| `assertion_item_declaration` | **No** | Assertions not supported |
| `;` (empty item) | **Yes** | Trivially |

### 7.2 Top-level `package_item` alternatives

| Spec item | Simulator | Notes |
|---|---|---|
| `anonymous_program` | **No** | Program not supported |
| `package_export_declaration` | **Yes** | Required for re-exporting imported names to downstream importers |
| `timeunits_declaration` | **No** | Simulator does not model time; spec also restricts this to repeating a prior declaration in the same time scope |

### 7.3 `data_declaration` detail

`data_declaration` covers four syntactic forms:

```
data_declaration ::=
    [ const ] [ var ] [ lifetime ] data_type_or_implicit list_of_variable_decl_assignments ;
  | type_declaration       // typedef
  | package_import_declaration
  | nettype_declaration
```

Only `type_declaration` and `package_import_declaration` are supported in this simulator.

`type_declaration` covers all `typedef` forms:
```
type_declaration ::=
    typedef data_type_or_incomplete_class_scoped_type type_identifier { variable_dimension } ;
  | typedef interface_port_identifier constant_bit_select . type_identifier type_identifier ;
  | typedef [ forward_type ] type_identifier ;   // forward declaration
```

**Forward typedef:** the third form (`typedef [ forward_type ] type_identifier ;`) is a
forward declaration, announcing that a name will be defined as a type later. The full
definition must appear before the forward typedef is actually used in an expression or
port/variable declaration. Forward typedefs exist to break declaration-order dependencies
(e.g. two types that mutually reference each other). The implementation must handle
forward typedefs with a two-pass or deferred resolution strategy: record the forward
declaration on first pass, then resolve all uses only after the full definition is seen.
Encountering a use before the full definition is a compile error.

**Typedef enum note:** importing a typedef for an enum type does **not** automatically
import the enum's literals. Given `import pkg::color_t`, the literals `RED`, `GREEN`,
`BLUE` remain invisible unless also explicitly imported or accessed as `pkg::RED`.

---

## 8. STATUS

### Implemented

| Feature | Notes |
|---|---|
| Package declarations (`package … endpackage`) | Extracted as `UnresolvedPackage`; compiled before any module |
| `typedef enum` inside a package | The only package item currently supported |
| Compilation-unit-scope wildcard import (`import pkg::*;`) | Collected at extraction time; applied to every module in the compilation unit |
| Compilation-unit-scope explicit import (`import pkg::name;`) | Same mechanism |
| Module-header wildcard and explicit imports (`module M import pkg::*; …`) | Parsed from `ModuleHeader.imports`; applied per-module |
| Qualified type reference in port/signal declarations (`pkg::my_type x`) | Handled in `resolveType` via `ScopedName` → `PackageRegistry` lookup |
| Qualified constant reference in expressions (`pkg::MEMBER`) | Handled in `buildExprDFG` via new `ScopedName` case |
| Qualified type cast (`pkg::my_type'(expr)`) | Handled in the cast-expression path |
| Multiple wildcard imports of the same package | Harmless (idempotent overwrite) |

### Deferred (not yet implemented)

| Feature | Reason |
|---|---|
| Body-level imports inside a module body (§2.3) | Not needed by current tests; deferred |
| Package `export` declarations (§5) | Deferred; no current test exercises re-export |
| `localparam`/`parameter` inside packages | Deferred; no current test exercises package-level constants |
| Functions and tasks inside packages | Marked "Not yet" in the simulator feature table |
| Package importing from another package | Deferred; `PackageImportDeclaration` inside a package body is silently ignored |
| Circular import detection | No enforcement yet; forbidden by spec but no validator |
| `std` built-in package | Not implemented; `std::` references will throw "Unknown package: std" |

---

## 7. The `std` built-in package

SystemVerilog provides a built-in package `std` that is implicitly wildcard-imported
into every compilation-unit scope. Its contents (defined in Annex G) are therefore
directly available in any scope unless redefined by user code. It can also be
accessed explicitly:

```systemverilog
std::some_type x;
std::some_func();
```
