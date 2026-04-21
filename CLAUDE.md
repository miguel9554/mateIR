# Project Instructions

## Project description

Verilog compiler restricted to synchronous logic only, intended as a stepping
stone toward a better custom HDL language.

Each RTL SystemVerilog file has a companion `.domains.yaml` file that carries
metadata Verilog cannot represent (clock/reset/IO domain info).

## Core data structure: mateir

The compiler lowers source files into **mateir**. In C++ this is represented by
`MateIR`, whose top-level design object is `Module`. `Module` contains:

- **Inputs/outputs/signals**: all ports and internal signals, with name and type
- **Flops**: all memory elements in the design
- **DFG**: a purely combinational DAG representing the register-transfer logic.
  Flops appear only as `.d` (sink) and `.q` (source) nodes. Clock and reset do
  NOT appear in the DFG.
- **Modules**: submodule instantiations

**The DFG is not the source of truth.** It must always coincide with `Module`,
but when iterating over inputs, outputs, signals, or flops, always iterate over
the `Module` containers — never the DFG maps.

## General guidelines

- Priority is **correctness and code simplicity**. Prefer simpler implementations
  unless performance is unreasonably bad.
- **NEVER sacrifice correctness.** If you're unsure, ask.
- **Never fail silently.** On any error or ill condition, throw an exception immediately.

### Scope of requests

Do **only what you are asked to do**. If you notice unrelated problems after
completing a task, ask for permission before touching them. Do not speculatively
fix other issues.

### Testing changes

If while testing your change you encounter an unrelated error (in a verilog
file, an unimplemented feature, etc.), do NOT fix it without permission. Ask first.

### Source of truth

Before designating any file or structure as the source of truth for something,
ask. Do not make that decision unilaterally.

## Slang library rules

This project uses `external/slang/` for SystemVerilog parsing.

### Allowed (read freely)
- `external/slang/include/slang/syntax/SyntaxTree.h`
- `external/slang/include/slang/syntax/SyntaxVisitor.h`
- `external/slang/scripts/syntax.txt` — **grep for specific types only**, never read in full (~2400 lines)

### Never read (unless explicitly requested)
- Generated files (`AllSyntax.h`, `SyntaxKind.h`) — use the mapping rules below
- Any `*.cpp`, test, or documentation files under `external/slang/`
- Any other slang file — ask first

### syntax.txt → C++ mapping

`syntax_gen.py` generates into `build/external/slang/source/slang/syntax/`.

**Naming:** append `Syntax` for class, bare name for enum.
- `ModuleDeclaration` → class `ModuleDeclarationSyntax`, enum `SyntaxKind::ModuleDeclaration`

**Inheritance:** `base=X` → inherits `XSyntax`. No base → inherits `SyntaxNode`.
`final=false` → abstract base, no own SyntaxKind.

**Members:**

| syntax.txt              | C++ type                            |
|-------------------------|-------------------------------------|
| `token x`               | `Token x;`                          |
| `tokenlist x`           | `TokenList x;`                      |
| `Foo x`                 | `not_null<FooSyntax*> x;`           |
| `Foo? x`                | `FooSyntax* x;` (nullable)          |
| `list<Foo> x`           | `SyntaxList<FooSyntax> x;`          |
| `separated_list<Foo> x` | `SeparatedSyntaxList<FooSyntax> x;` |

**multiKind:** when `multiKind=true`, the `kindmap` block lists SyntaxKind
values sharing that struct. Constructor takes `SyntaxKind kind`; check
`node.kind` to distinguish.

**Visitor:** implement `handle(const XSyntax&)` in a `SyntaxVisitor<Derived>`
subclass. One handle catches all kinds in a multiKind's kindmap.

**Common abstract bases:** `ExpressionSyntax`, `DataTypeSyntax` (extends
Expression), `StatementSyntax`, `MemberSyntax`, `TimingControlSyntax`,
`NameSyntax` (extends Expression).

## Domains file schema

Schema: `src/domains.schema.json` — read this file when working with `.domains.yaml` files or `io_domains_set.h/.cpp`.

## Project structure

```
src/
├── main.cpp                     CLI, pass orchestration, analyze/simulate modes
├── mateir/
│   ├── mateir.h                 MateIR result wrapper
│   ├── module.h/.cpp            Module, Signal, Param, FlopInfo, bindings
│   ├── types.h/.cpp             Type, Dimension, TypeKind, SyncKind
│   ├── dfg.h/.cpp               DFGNode, DFGOp, DFG container
│   └── debug.h                  Debug node selection types
├── frontends/
│   └── systemverilog/
│       ├── unresolved.h/.cpp    UnresolvedSignal/Param/Module (raw slang syntax pointers)
│       ├── syntax_helpers.h/.cpp
│       ├── systemverilog_frontend.h/.cpp
│       └── passes/              Elaboration and DFG/domain cleanup passes
├── consumers/
│   ├── sim/                     Simulation consumer
│   └── static_analysis/         Static analysis consumer
├── sim/
│   └── simulator.h/.cpp         Cycle-accurate simulator (CSV stimuli → CSV outputs)
└── util/
    ├── debug.h                  DFG dot/JSON dump helpers
    ├── source_loc.h             SourceLoc type
    ├── source_loc_resolve.h     Resolve slang tokens → SourceLoc
    └── syntax_helpers.h/.cpp   Misc slang utilities

tools/
└── vcd-compare/                 Standalone VCD comparison tool

tests/<testname>/
├── rtl/                         Verilog source + *.domains.yaml files
└── work/custom-sim/
    ├── Makefile
    ├── custom-sim.args
    ├── inputs/                  Per-signal CSV stimulus files
    └── outputs/                 Expected output CSVs
```

## Debug output and inspection tools

After each pass: `debug_output/<module>/<N>_<pass_name>.{dot,json}`

Use the skills `/read-dfg`, `/read-hierarchy`, and `/read-vcd` for inspecting
debug output. Each skill documents the file format, available commands, and
typical debugging workflows.

**Never write throwaway scripts for one-off queries.** Add new query types as
named commands to the relevant `tools/*.py` tool instead.
