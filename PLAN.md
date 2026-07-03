# Mate DPI Model ABI Plan

## Goal

Move the generated DPI wrapper off compiler/runtime C++ types so a testbench can link either:

- the current MateIR interpreter engine in phase 1, or
- a generated native model engine in phase 2.

The stable boundary is a C ABI. `SimValue`, `RuntimeInputUpdate`, `RtlRuntimeModel`,
frontend types, STL containers, and C++ exceptions must not cross it.

## Phase 1: Freeze And Use The ABI With The Interpreter

Phase 1 is not only a header freeze. The ABI itself is frozen in this phase, and the
existing interpreter is moved behind it. The generated DPI glue must include only the
public ABI plus `svdpi.h`; it must stop depending on `mate::dpi::DpiInstanceContext`,
`DpiInputBinding`, `SimValue`, `RuntimeInputUpdate`, or frontend config types.

Implementation steps:

1. Add `src/abi/mate_model_abi.h`.
   - C-only public contract.
   - Opaque `MateModel` and `MateInstance`.
   - Explicit status return on every fallible call.
   - Raw little-endian `uint64_t` word buffers for values.
   - Separate input, output, clock-domain, and reset-domain handles.

2. Add interpreter ABI implementation.
   - `src/abi/abi_interpreter.cpp` implements all generic ABI calls over
     `RtlRuntimeModel` and `RtlRuntimeInstance`.
   - `src/abi/abi_interpreter.h` exposes only a private C++ helper for generated
     phase-1 model code to create a `MateModel` from today’s compile config.
   - All C ABI calls catch exceptions and return `MateStatusCode`; exceptions never
     cross the ABI.

3. Split generated C++ output.
   - `<module>_dpi.cpp`: stable DPI glue that talks only to `mate_model_abi.h`.
   - `<module>_model.cpp`: phase-1 model implementation that embeds source files,
     domain files, parameters, and implements the model-specific `mate_model_create`.

4. Keep generated SystemVerilog stable initially.
   - `<module>_dpi.sv` and `<module>_dpi_pkg.sv` should remain behaviorally unchanged.
   - The existing event split for data changes, clock edges, and reset edges remains
     the SystemVerilog scheduling surface.

5. Update build rules.
   - Link the new ABI interpreter library.
   - Compile both generated C++ files.
   - Phase 1 still links frontend, yaml, slang, and runtime compiler because
     `mate_model_create` still compiles SV at simulation startup.

Phase-1 validation:

- Build `mate-dpi-codegen`.
- Regenerate and run at least one existing DPI-vs-RTL test.
- Then expand to representative tests with vectors, structs, enums, and reset/clock behavior.

## Current State

Phase 1, Phase 2A, Phase 2B, Phase 2C, Phase 2D, and Phase 2E are complete.

Current architecture:

- Public ABI: `src/abi/mate_model_abi.h`.
- Shared, frontend-free metadata types: `src/abi/generated_model_metadata.h`
  (`GeneratedInputMetadata`, `GeneratedOutputMetadata`, `GeneratedClockMetadata`,
  `GeneratedResetMetadata`, `GeneratedStorageMetadata`,
  `GeneratedCombinationalEvaluateFn`, `GeneratedResetApplyFn`,
  `GeneratedClockCommitFn`, `GeneratedFlopsInitFn`, `GeneratedModelMetadata`).
  This header only includes `mate_model_abi.h` and `sim/sim_value.h` — no
  mateir/frontend/slang — and is shared by both ABI backends below.
- Two ABI backends, same public entry points (`mate_model_create`,
  `mate_apply_clock`, ...), never linked into the same binary:
  - **Native backend**: `src/abi/abi_native.h` + `src/abi/abi_native.cpp`
    (library `mate-abi-native`, depends only on `mate-sim-value`). Builds
    `MateModel`/`MateInstance` purely from `GeneratedModelMetadata` — no
    `RtlRuntimeModel`/`RtlRuntimeInstance`, no SV compilation, ever. This is
    the backend linked into DPI simulation binaries
    (`tests/common/verilator.mk`'s `MATE_LIBS`).
  - **Interpreter backend**: `src/abi/abi_interpreter.h` +
    `src/abi/abi_interpreter.cpp` (library `mate-abi-interpreter`, depends on
    `mate-rtl-runtime-compiler` → frontend/slang/yaml-cpp). Compiles SV via
    `compileRtlRuntimeModel` inside `mate_model_create`/`createInterpreterModel`.
    Kept for the (currently unused) interpreter-only `createInterpreterModel`
    path; not exercised by generated code or the DPI link anymore.
- `mate-sim-value` (`src/sim/sim_value.{h,cpp}`) was split out of
  `mate-rtl-runtime` specifically so the native backend could depend on
  `SimValue` without pulling in `mate-mateir` (and, transitively via its
  private link to `slang::slang`, slang itself) at DPI link time.
- Generated DPI glue: `<module>_dpi.cpp`.
  - Includes only `abi/mate_model_abi.h` and `svdpi.h`.
  - Uses opaque `MateModel` / `MateInstance`, integer handles, status codes,
    clock/reset handles, and raw `uint64_t` word buffers.
  - Unchanged by Phase 2E — it never referenced the interpreter directly.
- Generated model file: `<module>_model.cpp` (`makeNativeModelCpp` in
  `tools/mate-dpi-codegen/main.cpp`).
  - `#include "abi/abi_native.h"` (not `abi_interpreter.h` — dropped in 2E
    along with the `InterpreterModelConfig`/source-file/domain-file/parameter
    embedding it used to build, since there is no SV to compile anymore).
  - Emits static generated ABI metadata tables for inputs, outputs, clocks,
    and resets.
  - Emits native straight-line combinational evaluation
    (`evaluateCombinational`) for the DFG's topological order.
    - The topo order is split into fixed-size chunk functions
      (`kCombinationalChunkSize` in `tools/mate-dpi-codegen/main.cpp`,
      currently 50 nodes/function) instead of one flat function. A single
      function over a large design's full node count (hundreds of thousands
      of statements for `ibex_core`) made C++ compiler register allocation
      and instruction scheduling blow up to the point of not finishing.
    - Every intermediate DFG node value is backed by a per-instance
      `temporaries` scratch span (sized to the total node count), not a
      local C++ variable, so values are addressable across chunk function
      boundaries.
    - Flop D/Q and other runtime-observable storage still flows through the
      existing `storage` span; `temporaries` is purely a codegen-internal
      concept with no interpreter-side counterpart to validate against.
  - Emits, per clock domain, a native `applyClockDomainN(inputs, storage)`
    FlopD -> FlopQ commit function, and per reset domain, a native
    `applyResetDomainN(storage)` reset-value-apply function. See
    `makeNativeFlopCommitCpp` in `tools/mate-dpi-codegen/main.cpp`.
    - A clock domain's commit skips a flop if any of its reset domains is
      currently at its active level (checked by reading that domain's source
      signal straight out of `inputs[]`, no persisted "reset active" flag) —
      this reproduces `MateIRRuntime::applyClockDomain`'s async-reset-wins
      priority (src/sim/runtime.cpp) without any interpreter-side map lookup.
  - `makeNativeFlopCommitCpp` also emits `initFlops(storage, mode, rng)`
    (`GeneratedFlopsInitFn`): applies `MateFlopsInitial` to every flop Q leaf
    in declaration order, mirroring `MateIRRuntime::initFlops`. Only the
    `ZERO` mode is exercised by generated testbenches today (`gen_tb.py`
    hardcodes `MATE_FLOPS_INITIAL_ZERO` at `mate_instance_init` time), but all
    three modes are implemented for API completeness.
- `abi_native.cpp` (the backend actually linked into DPI sims):
  - `mate_model_create`/`createNativeModel` build `MateModel` directly from
    `GeneratedModelMetadata`, with no second data source to cross-validate
    against (unlike the interpreter backend, which validated generated
    metadata against a compiled `RtlRuntimeModel`). Correctness now rests
    entirely on the codegen tool being correct — acceptable since metadata and
    native code are always generated together by the same tool run.
  - `mate_instance_init` applies `flops_init` for the requested
    `MateFlopsInitial` mode, writes the async/sync input updates straight into
    `native_inputs`, then applies `reset_apply` for every reset domain that
    reads active given those now-set inputs (mirrors
    `MateIRRuntime::initializeInputsAndEvaluate`'s reset-at-power-up), then
    evaluates combinational logic once.
  - `mate_apply_clock`/`mate_apply_reset`/`mate_set_input` are unconditionally
    native (there is no interpreter fallback in this file at all) — same
    logic as the native branches added to `abi_interpreter.cpp` in Phase 2D,
    ported over.
  - Low-level word/SimValue packing helpers (`wordsToSimValue`,
    `copyWordsToStorage`, `simValueToStorage`, `storageToWords`, `wordCount`,
    `guard`/`setOk`/`setError`) are intentionally duplicated from
    `abi_interpreter.cpp` rather than shared: the interpreter version is keyed
    on `mate::Type` (mateir), and sharing it here would have pulled mateir
    back into the native link. The native version uses plain
    `(int32_t width, bool is_signed)` pairs instead.
- `tests/common/verilator.mk`'s `MATE_LIBS` (used by every DPI test) now
  links only `libmate-abi-native.a`, `libmate-sim-value.a`, and
  `libfmt.a` — `libmate-abi-interpreter.a`, `libmate-rtl-runtime-compiler.a`,
  `libmate-systemverilog-frontend.a`, `libmate-rtl-runtime.a`,
  `libmate-mateir.a`, `yaml-cpp`, and slang are gone from the DPI simulation
  link entirely (verified via the generated `Vtb.mk`'s link line, not just by
  inspecting the Makefile template). The codegen step itself (`mate --dpi-lib`,
  see below) is unaffected and still links the full frontend/slang to compile
  SV and produce `<module>_model.cpp`/metadata in the first place — that's a
  build-time tool dependency, not a simulation-time one.

Last clean validation for Phase 2E:

- `scripts/docker-run.sh make dev`
- `scripts/docker-run.sh make -C tests/ibex_core/work/verilator clean simulate DPI=1`
  (~5.4 minutes end-to-end, same ballpark as Phase 2D — no measurable
  regression or improvement from dropping the interpreter link, as expected
  since Phase 2D had already removed the interpreter from the hot path)
- `scripts/docker-run.sh python tests/regression.py --mode verilator-dpi`
  (the actual regression entry point; `make regression` does not reliably
  exercise the same DPI path)
- Full regression result: `126/126 passed` under `--mode verilator-dpi`,
  `138/138 passed` under `make regression` (`tests/regression_tests.txt`
  currently excludes `ibex_core` for run-time reasons; verified separately
  above). Passed on the first attempt with the reduced link.

## Post-2E: `mate --dpi-lib` — Compiled-Library Packaging

Phase 2E made the DPI simulation link frontend/slang-free, but the consuming
simulator (Verilator) still compiled `<module>_dpi.cpp`/`<module>_model.cpp`
itself, which meant any SV simulator wanting to integrate a generated model
needed to know mate's own include paths, C++ standard, and which of mate's
static libraries to link. The actual integration boundary for "any simulator
wanting to use mate" should be: hand over SV wrapper files, get back one
compiled library, done.

What changed:

- The former standalone `tools/mate-dpi-codegen` binary is gone. Its codegen
  logic moved into `src/dpi_codegen/dpi_codegen.{h,cpp}` (function
  `generateDpiCodegen`) and is now a mode of the main `mate` binary:
  `mate --dpi-lib --top <module> --domains <file> --output-dir <dir>
  --module-name <name> --function-prefix <prefix> <source files...>`. This
  reuses `mate`'s existing frontend compile (the same `mateir`/`RtlRuntimeModel`
  already built for `--simulate`/`--analyze`) rather than compiling SV twice.
- New: `mate --dpi-lib` also compiles the generated `<module>_dpi.cpp` and
  `<top_module>_model.cpp` and combines their object files with the object
  members of caller-specified static libraries (`--dpi-link-libs`, typically
  `libmate-abi-native.a` + `libmate-sim-value.a`) into **one** self-contained
  output static library (`--dpi-out-lib`), via `src/dpi_codegen/dpi_lib_link.{h,cpp}`
  (`linkDpiLib`). Compilation and archiving are done by shelling out to
  `$CXX`/`$AR` (`--cxx`/`--ar`, default `c++`/`ar`); library objects are
  extracted into per-archive temp subdirectories (to avoid member-name
  collisions across input libraries) before being folded into the final `ar
  rcs` invocation.
- `svdpi.h` comes from `external/slang/external/ieee1800/svdpi.h` — the
  vendored IEEE 1800 LRM standard DPI header, not a Verilator-specific one —
  so compiling the generated glue doesn't require any particular target
  simulator's own headers.
- `tests/common/verilator.mk` now calls `$(BUILD_DIR)/mate --dpi-lib ...`
  instead of `$(BUILD_DIR)/mate-dpi-codegen ...`, and Verilator's build no
  longer takes `$(GEN_CPP)`/`$(GEN_MODEL_CPP)` as extra sources or needs
  `-CFLAGS` at all — it only adds the two generated `.sv` files to its SV
  sources and links the one produced `$(DPI_LIB)` via `-LDFLAGS`.
- Static library only, for now (per explicit decision) — a `.so` output
  (closer to the `-sv_lib` dynamic-loading convention used by VCS/Xcelium/
  Questa) is a natural follow-up once this path is validated further, but is
  out of scope here.

Validation: `arithmetic_ops` end-to-end via a manual `mate --dpi-lib` +
`make simulate DPI=1` invocation (confirmed the produced `.a` has exactly the
expected four object members: `dpi.o`, `model.o`, `abi_native.cpp.o`,
`sim_value.cpp.o`, and that Verilator's own link line references only that
one `.a`), then full regression: `126/126 passed` under
`python tests/regression.py --mode verilator-dpi`, `138/138 passed` under
`make regression`, and `ibex_core` separately at 100% DPI-vs-RTL match — now
in **~2.3 minutes** end-to-end (down from ~5.4 minutes), since Verilator's own
build no longer compiles the large generated `model.cpp` itself.

### Follow-up: parallel chunk-file compilation

`mate --dpi-lib` doing the compile itself (instead of leaving it to
Verilator's own single-invocation build) opened up a further win: the
combinational evaluator's `evaluateCombinationalChunkN` functions
(`kCombinationalChunkSize` = 50 nodes/function, see Phase 2C) are
independent — but they were all still emitted into one `<top_module>_model.cpp`
translation unit, so one `c++ -c` process compiled all of them sequentially on
one core regardless of that independence.

`makeNativeCombinationalCpp` (`src/dpi_codegen/dpi_codegen.cpp`) now splits
the chunk functions across `nativeCombinationalFileCount(chunk_count)`
sibling files, `<top_module>_model_chunk_N.cpp`, targeting
`2 * std::thread::hardware_concurrency()` files (capped at `chunk_count` so
small designs don't get more files than functions):

- Each chunk file gets its own private copy of the small
  `nodeWidth`/`maskToWidth`/`boolValue`/`widenForArithmetic`/`useSignedCompare`
  helpers (in an anonymous namespace — internal linkage, no ODR conflict
  across files), and defines its subset of `evaluateCombinationalChunkN` at
  **external** linkage (not anonymous-namespace) so the dispatcher in the main
  `<top_module>_model.cpp` can call them across translation units.
- The main `<top_module>_model.cpp` gets `extern` prototypes for every chunk
  function plus the small `evaluateCombinational()` dispatcher (still
  anonymous-namespace, still same-TU as `mate_model_create` so `&evaluateCombinational`
  resolves) — it also needs its own copy of the helpers, since the
  dispatcher's final output-write statements call `maskToWidth` directly.
- `DpiCodegenOutput::model_cpps` now carries the full list of generated model
  translation units (main + all chunk files); `DpiLibLinkConfig::sources`
  takes that full list instead of deriving two hardcoded paths from
  `module_name`/`top_module`.
- `dpi_lib_link.cpp`'s `linkDpiLib` compiles every source concurrently via
  `std::async(std::launch::async, ...)` per file rather than sequentially,
  then archives all resulting objects (plus the extracted `mate-abi-native`/
  `mate-sim-value` members) into the same single output `.a` as before —
  the caller-facing contract (`mate --dpi-lib` in, one `.a` out) is unchanged.
  Needs `Threads::Threads` linked into `mate` (added to `CMakeLists.txt`).

Validation: `arithmetic_ops` manual run confirmed 4 chunk files (on the local
14-core dev machine, `2*nproc` capped by actual chunk count) plus
`dpi.o`/`model.o`/`abi_native.cpp.o`/`sim_value.cpp.o` in the final archive,
and 100% DPI-vs-RTL match. Full regression: `126/126` under `--mode verilator-dpi`,
`138/138` under `make regression`. `ibex_core` (30 generated `.cpp` files: 28
chunk files + dpi.cpp + model.cpp) end-to-end in **~1m41s**, down from ~2.3
minutes before parallel compilation and ~5.4 minutes before `mate --dpi-lib`
existed at all.

**Follow-up correction — contiguous chunk-index file assignment was badly
imbalanced.** After landing the above, `ibex_core`'s wall-clock time and CPU
utilization didn't improve as much as the file count suggested it should
(cores visibly idle mid-build). Root cause: `evaluateCombinationalChunkN`
functions are bounded by *node count* (`kCombinationalChunkSize` = 50), not by
*generated code size* — a single node (e.g. a wide MUX) can expand into a
switch with tens of thousands of `case` labels, so two 50-node chunks can
differ in compiled size by orders of magnitude. Assigning contiguous
chunk-index ranges to files let a run of adjacent oversized chunks (which
cluster together, since topologically-close nodes tend to come from the same
RTL construct — e.g. a register-file read mux) all land in the *same* file:
one measured case had a single file balloon to 325,601 lines (one function
alone had 94,208 `case` labels) against ~11,683 lines for every other file,
so that one file single-handedly dominated wall-clock while every other
core sat idle after finishing early.

Fix: `makeNativeCombinationalCpp` now generates each chunk's text into its
own buffer first, then bin-packs chunks onto files by text size — largest
chunk first, always onto whichever file currently has the least accumulated
text (the "longest processing time" multiprocessor-scheduling heuristic) —
instead of contiguous index ranges. This cannot fully eliminate the
imbalance (the single 94k-line function is still one atomic, unsplittable
translation-unit-internal compile no matter which file it's binned into),
but it tightens the spread dramatically: `mate --dpi-lib`'s codegen+compile
step on `ibex_core` dropped from ~52s to ~33s, and the file holding that
giant function (94,444 lines after rebalancing) became the *only* meaningful
straggler (~9s behind the rest) instead of one of many serialized stragglers
finishing progressively later. Full `ibex_core` end-to-end: ~1m20s (down from
~1m41s). 126/126 regression still passing.

**Remaining known limit (not fixed, flagged for whoever picks this up
next):** a single oversized MUX/CONCAT node can still dominate one file's
compile time, since chunking splits by *node count*, not by each node's own
generated-code size. Fully closing that gap would mean also splitting a
single node's generated switch statement across multiple sub-functions (e.g.
an outer dispatcher over arm-value sub-ranges calling one of several
per-range switch functions) — a more invasive change to the MUX/CONCAT
codegen itself, not just the file-packing layer, and out of scope for this
pass.

## Phase 2: Native Generated Model

Replace `<module>_model.cpp` with native generated code implementing the same ABI.

The generated model owns:

- static metadata tables for inputs, outputs, clocks, and resets,
- instance storage for inputs, outputs, flops, and temporaries,
- straight-line combinational evaluation,
- reset and active clock-edge flop commits.

The unchanged pieces should be:

- `mate_model_abi.h`,
- `<module>_dpi.cpp`,
- `<module>_dpi.sv`,
- `<module>_dpi_pkg.sv`,
- testbench code.

Phase 2 is split into regression-clean subphases. Each subphase must end at a
100% clean DPI-vs-RTL regression point before the next subphase starts. "Mostly
passing" is not a valid handoff state. If a subphase exposes unrelated failures,
stop and classify them before mixing those fixes into codegen work.

### Phase 2A: Generated Metadata Model

Generate `<module>_model.cpp` with static ABI metadata tables for inputs,
outputs, clocks, and resets. Evaluation still delegates to the interpreter.

Clean anchor:

- Generated metadata must match interpreter runtime metadata exactly.
- Full behavior should be identical to phase 1.
- DPI-vs-RTL regression must pass before 2B starts.

### Phase 2B: Native Storage, Interpreted Evaluation

Generate native `MateInstance` storage for ABI-visible inputs, outputs, flops,
and temporaries, while keeping expression evaluation mechanically close to the
interpreter.

Operational starting scope for a fresh agent:

- Start from `src/abi/abi_interpreter.cpp`, `src/abi/abi_interpreter.h`, and
  `tools/mate-dpi-codegen/main.cpp`.
- Introduce generated/native ABI-visible storage in `<module>_model.cpp` or in
  a model-owned structure passed through the ABI.
- Keep `RtlRuntimeInstance` as the behavioral golden evaluator during 2B.
- It is acceptable for native storage to mirror ABI-visible values while runtime
  evaluation still consumes the same updates and produces the same outputs.
- Do not implement straight-line DFG expression codegen in 2B. That belongs to
  Phase 2C.
- Do not remove frontend/slang/runtime-compiler links in 2B. That belongs to
  Phase 2E.

Clean anchor:

- ABI ingress/egress and instance lifecycle use generated storage.
- Visible behavior remains identical.
- `scripts/docker-run.sh make regression` must pass before 2C starts.

### Phase 2C: Straight-Line Combinational Codegen

Emit straight-line code for the combinational topo order, preserving the
operation semantics of `MateIRRuntime::evaluateNode`.

Clean anchor:

- Per-cycle graph walking is removed for combinational evaluation.
- Arithmetic, compare, select, concat, slice, signedness, and width semantics
  match the interpreter.
- DPI-vs-RTL regression must pass before 2D starts.

### Phase 2D: Native Clock/Reset Commit

Emit generated reset application and flop D-to-Q commits per clock/reset domain.

Clean anchor:

- Runtime scheduling maps are no longer in the hot clock/reset path.
- Active-edge, inactive-edge, sync-input sampling, and async reset behavior
  match phase 1.
- DPI-vs-RTL regression must pass before 2E starts.

### Phase 2E: Drop Frontend/Slang From DPI Link

Remove the phase-1 interpreter/compiler dependencies from DPI simulation.

Clean anchor:

- `libmate-rtl-runtime-compiler.a`, `libmate-systemverilog-frontend.a`,
  yaml-cpp, and slang are absent from the DPI simulation link.
- Generated model libraries remain reusable without recompiling SV at
  simulation startup.
- DPI-vs-RTL regression must pass with the reduced link.

## ABI Word Contract

All scalar/vector values crossing the ABI use raw `uint64_t` words:

- bit 0 is `words[0]` bit 0,
- words are little-endian by significance,
- `nwords == ceil(width / 64)`,
- unused high bits in the final word must be zero on input,
- outputs always zero unused high bits,
- the ABI is 2-state only.

The generated DPI glue remains responsible for rejecting SystemVerilog X/Z values before
passing values to the model ABI.
