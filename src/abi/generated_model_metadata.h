#pragma once

// Types describing the static shape and generated native functions of a
// compiled model. Shared by both ABI backends:
//   - abi_interpreter.{h,cpp}: interpreter-backed, requires the SV frontend.
//   - abi_native.{h,cpp}: fully native, no SV frontend/slang dependency.
// This header must stay free of any frontend/mateir/slang dependency so the
// native backend's link stays free of them too.

#include "abi/mate_model_abi.h"
#include "sim/sim_value.h"

#include <cstdint>
#include <random>
#include <span>
#include <string_view>
#include <vector>

namespace mate::abi {

struct GeneratedInputMetadata {
    std::string_view leaf_name;
    int32_t width = 0;
    bool is_signed = false;
    MateInputKind kind = MATE_INPUT_ASYNC;
    int32_t clock_id = -1;
    int32_t reset_id = -1;
};

struct GeneratedOutputMetadata {
    std::string_view leaf_name;
    int32_t width = 0;
    bool is_signed = false;
};

struct GeneratedClockMetadata {
    std::string_view display_name;
    std::string_view source_leaf_name;
    MateEdge active_edge = MATE_EDGE_POSEDGE;
};

struct GeneratedResetMetadata {
    std::string_view display_name;
    std::string_view source_leaf_name;
    MateEdge active_edge = MATE_EDGE_POSEDGE;
};

enum class GeneratedStorageKind {
    Temporary,
    FlopD,
    FlopQ,
};

struct GeneratedStorageMetadata {
    GeneratedStorageKind kind = GeneratedStorageKind::Temporary;
    std::string_view full_path;
    std::string_view leaf_name;
    int32_t width = 0;
    bool is_signed = false;
};

using GeneratedCombinationalEvaluateFn = void (*)(
    std::span<const mate::SimValue> inputs,
    std::span<mate::SimValue> outputs,
    std::span<mate::SimValue> storage,
    std::span<mate::SimValue> temporaries);

// Applies reset values to a single reset domain's FlopQ storage slots.
using GeneratedResetApplyFn = void (*)(std::span<mate::SimValue> storage);

// Commits FlopD -> FlopQ storage for a single clock domain, honoring async
// reset priority (a flop is skipped if any of its reset domains is currently
// at its active level, read from `inputs`).
using GeneratedClockCommitFn = void (*)(
    std::span<const mate::SimValue> inputs,
    std::span<mate::SimValue> storage);

// Applies FlopsInitial to every flop Q leaf in the design, in generated
// (flop-declaration) order. Mirrors MateIRRuntime::initFlops.
using GeneratedFlopsInitFn = void (*)(
    std::span<mate::SimValue> storage,
    MateFlopsInitial mode,
    std::mt19937_64& rng);

struct GeneratedModelMetadata {
    std::vector<GeneratedInputMetadata> inputs;
    std::vector<GeneratedOutputMetadata> outputs;
    std::vector<GeneratedClockMetadata> clocks;
    std::vector<GeneratedResetMetadata> resets;
    std::vector<GeneratedStorageMetadata> storage;
    GeneratedCombinationalEvaluateFn evaluate_combinational = nullptr;
    // Scratch slots for intermediate DFG node values, private to the generated
    // combinational evaluator. Not part of runtime-observable metadata: these
    // values have no interpreter-side counterpart to validate against.
    size_t temporaries_count = 0;
    // One entry per `resets`/`clocks` entry, same order, when
    // `evaluate_combinational` is set. Empty when the model has no native
    // combinational evaluator (interpreter-only fallback).
    std::vector<GeneratedResetApplyFn> reset_apply;
    std::vector<GeneratedClockCommitFn> clock_commit;
    // Set alongside evaluate_combinational for native models.
    GeneratedFlopsInitFn flops_init = nullptr;
};

} // namespace mate::abi
