# Mate

Mate is an IR and compiler stack for synchronous hardware. Its core
representation, `MateIR`, models RTL as hardware structure rather than as an
event-driven simulator program: module hierarchy, ports, flops, global clock and
reset registries, and combinational dataflow are first-class parts of the IR.

The current frontend compiles a supported subset of SystemVerilog into
`MateIR`. The main consumers today are:

- a static analysis tool for inspecting the compiled design and validating
  clock/reset/domain facts;
- a DPI simulation backend that emits a SystemVerilog replacement module, a DPI
  package, and a native static library for use from Verilator or another
  SystemVerilog simulator.

## MateIR

`MateIR` is the project boundary between frontends and consumers. It contains
the top `Module` hierarchy, global clock/reset registries, and source metadata.
Each `Module` owns its ports, internal module nodes, flops, combinational DFG,
and submodule hierarchy.

The important hardware concepts are native to the IR:

- **Clock domains** are global registry entries with source signal and edge
  polarity.
- **Reset domains** are global registry entries with source signal and active
  polarity.
- **Flops** carry clock/reset domain IDs, reset values, and D/Q bindings.
- **Combinational logic** is represented as a DFG and checked for invalid
  dependency cycles.
- **Hierarchy** is preserved so consumers can reason about the structural design
  as well as the flattened data dependencies used by analysis and codegen.

The DFG is not the source of truth for module structure. Consumers that need
ports, signals, or flops should iterate the `Module` data structures and their
bindings.

## SystemVerilog Compiler

The SystemVerilog frontend accepts synchronous RTL and lowers it to `MateIR`.
The supported subset is intentionally hardware-oriented:

- sequential logic is represented as edge-triggered flops from `always_ff` or
  equivalent edge-triggered `always` blocks;
- combinational logic comes from `always_comb` and continuous assignments;
- packages, parameters, generate constructs, packed structs, arrays, hierarchy,
  and module instantiation are supported where they lower to the synchronous
  model;
- event-driven simulation constructs, latches, tri-state behavior, and
  unsupported dynamic SystemVerilog features are rejected.

Top-level clock, reset, synchronous input, and asynchronous input facts are
provided by a `.domains.yaml` sidecar or inferred with `--infer-top-domains`.
Intentional CDC synchronizer flops are provided by `.cdc.yaml` sidecars or
inferred with `--infer-synchronizers`.

Example compile:

```bash
scripts/docker-run.sh make dev

scripts/docker-run.sh ./build/dev/mate \
    --top counter_top \
    --domains tests/counter_top/rtl/counter_top.domains.yaml \
    tests/counter_top/rtl/counter.v \
    tests/counter_top/rtl/counter_top.v
```

Example with inferred top domains:

```bash
scripts/docker-run.sh ./build/dev/mate \
    --top counter_top \
    --infer-top-domains \
    --emit-inferred-domains counter_top.inferred.domains.yaml \
    tests/counter_top/rtl/counter.v \
    tests/counter_top/rtl/counter_top.v
```

Useful compiler flags:

- `--top <module>` selects the top module and is required for multi-file
  designs.
- `--domains <file>` loads top-level domain facts.
- `--infer-top-domains` infers top-level clock/reset/input domains instead of
  loading `--domains`.
- `--infer-synchronizers` infers CDC synchronizer flops instead of loading
  `.cdc.yaml` sidecars.
- `--params K=V,...` overrides top-level parameters.
- `--dump-passes` emits debug DFG artifacts under `debug_output/<top>/`.

## Static Analysis

The static analysis consumer runs after SystemVerilog lowering and frontend
validation. It prints a `MateIR` summary, clock/reset registries, module domain
usage, flops, ports, hierarchy, and optional debug views of selected DFG
dependencies.

Run it directly:

```bash
scripts/docker-run.sh ./build/dev/mate \
    --analyze \
    --top counter_top \
    --domains tests/counter_top/rtl/counter_top.domains.yaml \
    tests/counter_top/rtl/counter.v \
    tests/counter_top/rtl/counter_top.v
```

Run a test's static check:

```bash
scripts/docker-run.sh make -C tests/counter_top/work/static analyze
```

Helpful analysis flags:

- `--debug-node-deps <node,...>` prints direct DFG inputs for selected nodes.
- `--debug-node-paths <source=target,...>` prints one dependency chain between
  selected nodes.
- `--debug-nodes <module:node,...>` asks frontend debug dumps to focus on
  selected nodes.

## DPI Simulation Backend

The DPI backend turns compiled `MateIR` into simulator-ready artifacts:

- `<top>_dpi.sv`: a generated SystemVerilog module with the same external port
  shape as the compiled top, backed by DPI calls;
- `<top>_dpi_pkg.sv`: a generated package containing DPI imports and helper
  declarations;
- `<top>_dpi.a`: a native static library containing the generated C++ model and
  DPI glue.

Generate those artifacts with `--dpi-lib`:

```bash
scripts/docker-run.sh ./build/dev/mate \
    --dpi-lib \
    --top counter_top \
    --domains tests/counter_top/rtl/counter_top.domains.yaml \
    --output-dir tests/counter_top/rtl/generated \
    --module-name counter_top_dpi \
    --function-prefix mate_counter_top \
    --dpi-out-lib tests/counter_top/rtl/generated/counter_top_dpi.a \
    --dpi-include-dirs "$(pwd)/src,$(pwd)/external/slang/external/ieee1800" \
    --dpi-link-libs "$(pwd)/build/dev/libmate-abi-native.a" \
    tests/counter_top/rtl/counter.v \
    tests/counter_top/rtl/counter_top.v
```

The generated static library is self-contained apart from the simulator process
that links it. Verilator does not need to compile the generated C++ sources
itself; it only needs the generated SystemVerilog files on the source list and
the archive passed through linker flags.

### Using the Artifacts with Verilator

1. Build Mate and the ABI support library:

```bash
scripts/docker-run.sh make dev
```

2. Generate the DPI artifacts with `mate --dpi-lib`, as shown above.

3. Add the generated SystemVerilog package and module to the Verilator source
   list before the testbench instantiates the DPI-backed module.

4. Link the generated archive into the Verilator executable:

```bash
verilator --timing --cc --exe --build --main \
    --top-module tb \
    -Itests/counter_top/rtl \
    -LDFLAGS "$(pwd)/tests/counter_top/rtl/generated/counter_top_dpi.a" \
    tests/counter_top/rtl/generated/counter_top_dpi_pkg.sv \
    tests/counter_top/rtl/generated/counter_top_dpi.sv \
    tests/counter_top/tb/tb.sv
```

In the repository test harness, this flow is wrapped by the Verilator work
directories:

```bash
scripts/docker-run.sh make -C tests/counter_top/work/verilator simulate DPI=1
```

The harness regenerates the DPI package/module/archive, builds the Verilator
binary, runs it, and checks the DPI-backed model against the RTL testbench.

## Building

The canonical environment is the repository Docker image:

```bash
scripts/docker-build.sh
scripts/docker-run.sh make dev
```

The wrapper expects the `mate-dev:latest` image. Native builds use the same
targets:

```bash
make dev
make sanitized
make debug
make release
```

Slang is vendored as a submodule. If the local Slang install is missing or stale,
rebuild it before building Mate:

```bash
bash scripts/build_slang.sh
```

## Regression

Run the default regression:

```bash
scripts/docker-run.sh python tests/regression.py
```

Run the Verilator DPI regression:

```bash
scripts/docker-run.sh python tests/regression.py --mode verilator-dpi
```

Additional API-surface checks:

```bash
scripts/docker-run.sh python tests/check_dfg_api_surface.py
scripts/docker-run.sh python tests/check_module_node_api_surface.py
```

## Debug Artifacts

Compiler debug output is written under `debug_output/<top>/`, usually inside a
test work directory. Common artifacts include per-pass DFG `.json` and `.dot`
files, `hierarchy.json`, and flop snapshots around domain and flop passes.

Prefer the repository inspection tools over ad hoc scripts:

```bash
tools/dfg_inspect.py
tools/hierarchy_inspect.py
tools/vcd_inspect.py
```
