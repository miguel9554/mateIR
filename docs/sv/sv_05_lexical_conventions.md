# SystemVerilog Section 5 — Lexical Conventions: Support in This Compiler

This document describes how each feature of IEEE Std 1800-2023 §5 (Lexical
conventions) is handled by this compiler. The compiler targets **synchronous RTL
only**: it accepts synthesisable, clocked Verilog as input and lowers it into the
`Module` IR.

All section references below are to IEEE Std 1800-2023.

---

## General note: the role of slang

The compiler delegates the **entire lexical and preprocessing phase** to the
`external/slang` library. Slang tokenises source text, expands compiler directives,
and constructs the syntax tree before any compiler code runs. As a consequence, many
sub-clauses of §5 — white space, comment syntax, keyword lists, operator character
sequences — are transparently handled by slang and have no explicit policy in this
compiler beyond "whatever slang accepts".

Sub-clauses that fall entirely into this category are listed as
**Delegated to slang** in the summary table. The sections below describe only the
features that have additional compiler-level restrictions or mapping rules.

---

## 5.3 White space

**Delegated to slang.** Spaces, tabs, newlines, and formfeeds are insignificant
token separators. No compiler-level restriction.

---

## 5.4 Comments

**Delegated to slang.** Both comment forms are accepted:

```systemverilog
// one-line comment
/* block comment */
```

Block comments may not be nested (slang enforces this).

---

## 5.5 Operators

**Delegated to slang.** Operator recognition and precedence are handled during
parsing. The DFG ops that correspond to each operator are described in the §11 doc
(Operators and expressions).

---

## 5.6 Identifiers, keywords, and system names

### 5.6.1 Simple identifiers

**Supported.** A simple identifier is any sequence of letters, decimal digits,
dollar signs (`$`), and underscores (`_`) that does not begin with a digit or `$`.
Identifiers are **case-sensitive**. The compiler imposes no length limit beyond
slang's implementation limit (≥ 1024 characters per the spec).

### 5.6.2 Escaped identifiers

**Not supported.** Escaped identifiers (beginning with `\` and terminated by
white space) are rejected. All signal and module names in synthesisable RTL must be
simple identifiers.

### 5.6.3 Keywords

**Delegated to slang.** All SystemVerilog reserved keywords are recognised by
slang. The compiler does not maintain its own keyword list.

### 5.6.4 System tasks and system functions

System tasks and system functions (names beginning with `$`) are covered in the
§20/§21 documentation. Only the subset relevant to synthesisable RTL and
parameter-constant evaluation is supported.

### 5.6.5 Compiler directives

Compiler directives (backtick constructs such as `` `define ``, `` `ifdef ``,
`` `include ``) are processed entirely by slang's preprocessor before the syntax
tree is built. This compiler imposes no additional policy here. The directive set
and any restrictions are covered in the §22 documentation. One notable restriction
documented in §6 is that `` `default_nettype `` is not supported.

---

## 5.7 Numbers

### 5.7.1 Integer literal constants

**Supported**, with the following rules:

**Forms accepted:**

| Form | Example | Notes |
|---|---|---|
| Unsized decimal | `659` | Treated as signed 32-bit by slang |
| Sized decimal | `8'd42` | Size in bits, decimal value |
| Sized hexadecimal | `8'hFF` | Case-insensitive hex digits |
| Sized octal | `8'o77` | |
| Sized binary | `8'b1010_1010` | |
| Signed sized | `8'sd6`, `8'shFF` | `s` qualifier sets signed interpretation |

**Underscore separator:** the `_` character may appear anywhere in the digit
sequence except as the first character; it is ignored and used for readability.

**Sign prefix:** a unary `+` or `-` before a sized literal is a unary operator,
not part of the literal itself. Negative values are represented in two's-complement
form.

**4-state digits (`x`, `z`, `?`) are not supported.** The IR is 2-state (§6.3);
literals containing `x` or `z` digits (e.g. `4'b10x0`, `8'hzz`, `4'bz`) are not
accepted in synthesisable RTL context.

**Unbased unsized literals** `'0`, `'1` are supported (set all bits to 0 or 1).
`'x` and `'z` are not supported.

### 5.7.2 Real literal constants

**Supported as parameter expressions only.** Real literals in decimal notation
(`1.5`, `3.14`) or scientific notation (`1.2e3`, `0.5E-2`) are accepted when they
appear as parameter default values or in parameter constant expressions. They are
not valid in RTL signal expressions (not synthesisable).

The default type of a real literal is `real`; use `shortreal'(1.2)` to cast to
`shortreal`. See §6.12 in the data-types documentation.

---

## 5.8 Time literals

**Not supported.** Time literals (`2.1ns`, `40ps`) are interpreted as `realtime`
values scaled to the current timescale. The compiler has no concept of simulation
time and no `realtime` type. Any time literal in a synthesisable context is an
error.

---

## 5.9 String literals

**Supported as parameter expressions only.** Both single-line quoted strings
(`"hello"`) and triple-quoted multi-line strings (`"""..."""`) are accepted when
they appear as `parameter` or `localparam` string values. String literals are not
valid as RTL signal values. See §6.16 in the data-types documentation.

String escape sequences (`\n`, `\t`, `\\`, `\"`, `\xhh`, `\ddd`) are handled by
slang.

---

## 5.10 Structure literals

**Supported.** The assignment-pattern literal syntax `'{...}` (§5.10) is accepted
by the compiler in RTL assignments and expressions:

```systemverilog
// default-value pattern — set every element or field to a given value
arr = '{default: '0};

// positional pattern
arr = '{8'hAA, 8'hBB, 8'hCC};
```

Assignment patterns are most commonly used with unpacked arrays (see §7.4 in the
aggregate-types documentation). Their use with structure types is also syntactically
accepted; it will become fully meaningful once packed-struct support is implemented
(§7.2 in the aggregate-types documentation, currently planned).

Assignment patterns are **not** valid in variable declaration initialisers
(consistent with the restriction on initialiser expressions described in §6.8 of
the data-types documentation). They are valid in procedural and continuous
assignment statements.

---

## 5.11 Array literals

**Supported in assignment contexts.** Array literals use the same `'{...}` syntax
as structure literals (§5.10) but are interpreted in an array type context:

```systemverilog
logic [7:0] arr [3:0];

arr = '{8'hAA, 8'hBB, 8'hCC, 8'hDD};  // positional
arr = '{default: '0};                   // all elements to 0
arr = '{0: 8'h01, 1: 8'h02, default: '0};  // indexed with default
```

As with structure literals, array literals are valid in procedural and continuous
assignment statements but **not** in variable declaration initialisers. See §7.4
in the aggregate-types documentation for the array IR mapping.

---

## 5.12 Attributes

**Not supported.** Attribute instances (`(* attr_name = value *)`) are parsed by
slang but the compiler does not recognise or act on any attribute. No synthesis
directive, case-modifier, or tool-hint attribute has any effect. Attributes are
silently ignored.

---

## 5.13 Built-in methods

Built-in methods use dot-notation (`object.method()`). The supported set is
type-specific and documented alongside each type:

- **Enum methods** (`.first()`, `.last()`, `.next()`, `.prev()`, `.num()`,
  `.name()`): see §6.19 in the data-types documentation.
- **Array querying functions** (`$size`, `$left`, `$right`, etc.): these are
  system functions rather than methods and are documented in §20.7 / the §7
  aggregate-types documentation.

No other built-in method categories (string methods, queue methods, associative
array methods, dynamic-array methods, class methods) are supported in
synthesisable RTL context.

---

## Summary table

| §5 feature | Support |
|---|---|
| White space, comments | **Delegated to slang** |
| Operator tokens | **Delegated to slang** (see §11 doc) |
| Simple identifiers | **Yes** — case-sensitive |
| Escaped identifiers (`\name `) | **No** |
| Keywords | **Delegated to slang** |
| System tasks/functions (`$…`) | **Partial** — see §20/§21 doc |
| Compiler directives (`` `define ``, etc.) | **Delegated to slang** — see §22 doc |
| Integer literals (sized, all bases) | **Yes** |
| Integer literals with `x`/`z`/`?` digits | **No** — IR is 2-state |
| Unbased `'0`, `'1` | **Yes** |
| Unbased `'x`, `'z` | **No** |
| Real literals (RTL signals) | **No** |
| Real literals (parameters) | **Yes** |
| Time literals | **No** |
| String literals (RTL signals) | **No** |
| String literals (parameters) | **Yes** |
| Structure literals `'{...}` (assignments) | **Yes** |
| Structure literals `'{...}` (initialisers) | **No** |
| Array literals `'{...}` (assignments) | **Yes** |
| Array literals `'{...}` (initialisers) | **No** |
| Attributes `(* … *)` | **No** — silently ignored |
| Built-in methods | **Type-specific** — see per-type docs |
