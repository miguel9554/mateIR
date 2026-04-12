# Verilog → Circuit IR Compiler

A compiler that ingests synchronous RTL written in SystemVerilog/Verilog and
produces a structured, clock-aware circuit IR. It is not a general-purpose
Verilog simulator or synthesis tool — it only accepts designs that are valid
synchronous netlists, and rejects anything that falls outside that subset.

## Scope and design philosophy

The compiler targets a well-defined class of RTL: purely synchronous digital
circuits. The accepted subset is intentionally narrow:

- All sequential logic must be expressed as edge-triggered flip-flops inside
  `always_ff` (or `always @(posedge/negedge ...)`) blocks.
- Combinational logic lives in `always_comb` / `assign` blocks. It must be
  purely combinational — no latches, no clock or reset signals used as data.
- Asynchronous logic, tri-state buses, initial blocks, and event-driven
  simulation constructs are not supported and will be rejected.

The goal is correctness and machine-readability of the output IR, not broad
Verilog compatibility. Verilog is used as a familiar input syntax; the real
target is the internal circuit representation.

## The circuit IR

The output of compilation is a `ResolvedModule` tree. Each module contains:

- **Ports**: inputs and outputs with resolved types, clock/reset classification,
  and clock domain assignment.
- **Flops**: every flip-flop extracted from sequential blocks, with its clock
  signal, optional async reset signal and reset value, and data-path driver.
- **DFG** (Data Flow Graph): a purely combinational DAG of the register-transfer
  logic. Flops appear only as `.d` sink nodes and `.q` source nodes. Clock and
  reset signals are absent from the DFG — they are metadata on the flop, not
  graph edges.
- **Hierarchy**: submodule instantiations are resolved recursively and inlined
  into the top-level flat DFG for analysis, while the module hierarchy is
  preserved in the IR for structural information.

## Domain sidecar files

Verilog ports carry no clock or IO domain information. Each RTL module is
accompanied by a `.domains.yaml` sidecar file that supplies this metadata:

```yaml
module_name: counter_top

resets:
  a_rst:
    polarity: positive

clock_domains:
  clk:
    polarity: posedge
    inputs_outputs:
      - count
```

The schema is in `src/domains.schema.json`. Every port must be assigned to a
clock domain, a reset, or the async domain.

## Usage

The executable now has three stages: a frontend (`--frontend systemverilog` by
default), the mateIR pipeline, and optional consumers such as static analysis or
simulation. The current architecture is described in
`docs/mateir_architecture.md`.

**Compile a single-module design:**
```
./build/custom_hdl_compiler --domains module.domains.yaml module.v
```

**Serialize mateIR:**
```
./build/custom_hdl_compiler --emit-mateir out.mateir.json \
    --domains module.domains.yaml module.v
```

**Run static analysis:**
```
./build/custom_hdl_compiler --analyze --domains module.domains.yaml module.v
```

**Simulate a hierarchical design:**
```
./build/custom_hdl_compiler --simulate \
    --top counter_top \
    --inputs-dir tests/counter_top/work/custom-sim/stimuli \
    --output-dir output \
    --domains tests/counter_top/rtl/counter_top.domains.yaml \
             tests/counter_top/rtl/counter.domains.yaml \
    tests/counter_top/rtl/counter.v \
    tests/counter_top/rtl/counter_top.v
```

When multiple source files are provided, `--top` is required to identify the
root module. Submodules are resolved automatically from the same file set.

**Optional flags:**
- `--params KEY=VAL,...` — override top-module parameters
- `--flops-initial zeros|ones|random` — flip-flop initial state for simulation
- `--flops-seed N` — seed for random initialisation

## Building

Slang (the SystemVerilog parser) is a submodule that must be built and installed
before building the compiler:

```bash
bash scripts/build_slang.sh   # once, or after updating the slang submodule
```

Then build the compiler:

```bash
mkdir -p build && cd build
cmake ..
make -j$(nproc) custom_hdl_compiler
```

The build requires CMake 3.15+, a C++20 compiler, and the yaml-cpp library.

## Pass pipeline

The compiler runs the following passes in order on the top module's flat DFG:

| # | Pass | Description |
|---|------|-------------|
| 0 | elaboration | Verilog AST → ResolvedModule + DFG per module |
| 1 | dfg_inline | Inline submodule DFGs into the top-level flat DFG |
| 2 | concat_cleanup | Resolve temporary CONCAT_ALIGN nodes from partial LHS assigns |
| 3 | constant_fold | Fold constant expressions; simplify constant-selector MUXes |
| 4 | type_propagation | Infer bit-widths across the DFG |
| 5–8 | condition_normalization + constant_fold (×2) | Normalise and re-fold MUX conditions |
| 9 | flop_resolve | Identify clock/reset from `always_ff` triggers; strip reset MUX; tag ports |
| 10 | dce | Dead-code elimination |
| 11 | io_domains_set | Apply `.domains.yaml`; assign clock domains to all ports and flops |

After the pipeline, combinational dependency analysis validates that there are
no combinational loops in the design.

## Tests

Tests live under `tests/<name>/`. Each test has:
- `rtl/` — Verilog source and `.domains.yaml` sidecar files
- `work/custom-sim/` — simulation Makefile, input stimuli CSVs, expected output CSVs

Run a test:
```bash
make -C tests/counter_top/work/validate
```
