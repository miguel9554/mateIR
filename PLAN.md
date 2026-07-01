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

Phase 1, Phase 2A, Phase 2B, and Phase 2C are complete.

Current architecture:

- Public ABI: `src/abi/mate_model_abi.h`.
- Private interpreter bridge: `src/abi/abi_interpreter.h` and
  `src/abi/abi_interpreter.cpp`.
- Generated DPI glue: `<module>_dpi.cpp`.
  - Includes only `abi/mate_model_abi.h` and `svdpi.h`.
  - Uses opaque `MateModel` / `MateInstance`, integer handles, status codes,
    clock/reset handles, and raw `uint64_t` word buffers.
- Generated phase-2 model file: `<module>_model.cpp`.
  - Implements model-specific `mate_model_create`.
  - Embeds source/domain/parameter config for the interpreter.
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
      boundaries. See `GeneratedCombinationalEvaluateFn` in
      `src/abi/abi_interpreter.h` and `MateInstance::native_temporaries` in
      `src/abi/abi_interpreter.cpp`.
    - Flop D/Q and other runtime-observable storage still flows through the
      existing `storage` span; `temporaries` is purely a codegen-internal
      concept with no interpreter-side counterpart to validate against.
- `abi_interpreter.cpp` currently:
  - compiles SV into `RtlRuntimeModel`,
  - validates generated ABI metadata against runtime metadata,
  - stores generated ABI metadata in `MateModel`,
  - resolves ABI handles from generated metadata,
  - maps ABI handles to runtime ids,
  - refreshes FlopQ/Internal/FlopD storage from `RtlRuntimeInstance` before
    calling the generated `evaluate_combinational`,
  - delegates clock edges, reset edges, and flop commit to
    `RtlRuntimeInstance` (native clock/reset commit is Phase 2D's job).

Last clean validation for Phase 2C:

- `scripts/docker-run.sh make dev`
- `scripts/docker-run.sh make -C tests/ibex_core/work/verilator clean simulate DPI=1`
  (~10 minutes end-to-end; dominated by compiling the generated
  `ibex_core_model.cpp`; previously did not finish at all with a single flat
  `evaluateCombinational`)
- `scripts/docker-run.sh make regression`
- Full regression result: `139/139 passed`.

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
