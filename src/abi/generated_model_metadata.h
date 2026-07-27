#pragma once

// Types describing the static shape and generated native functions of a
// compiled model. This header must stay free of any frontend/mateir/slang
// dependency so the native backend's link stays free of them too.

#include "abi/mate_model_abi.h"

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

// One entry per observable *name*, as opposed to `GeneratedStorageMetadata`
// which has one entry per physical storage slot. Multiple names can alias
// the same slot (e.g. a submodule output port wired straight through to an
// internal wire) — each keeps its own full_path here but shares
// storage_index, so the ABI can enumerate every name without duplicating the
// underlying storage or the codegen write for it.
struct GeneratedObservableMetadata {
    GeneratedStorageKind kind = GeneratedStorageKind::Temporary;
    std::string_view full_path;
    std::string_view leaf_name;
    int32_t width = 0;
    bool is_signed = false;
    size_t storage_index = 0;
};

struct NativeWordSlot {
    uint64_t* words = nullptr;
    int32_t nwords = 0;
};

// `spills` is a single contiguous word buffer of `spill_words_count` words;
// the generated chunks address it via compile-time offsets, so it carries no
// per-value slot metadata.
using GeneratedCombinationalEvaluateFn = void (*)(
    std::span<const NativeWordSlot> inputs,
    std::span<NativeWordSlot> outputs,
    std::span<NativeWordSlot> storage,
    std::span<uint64_t> spills);

// Applies reset values to a single reset domain's FlopQ storage slots.
using GeneratedResetApplyFn = void (*)(std::span<NativeWordSlot> storage);

// Commits FlopD -> FlopQ storage for a single clock domain, honoring async
// reset priority (a flop is skipped if any of its reset domains is currently
// at its active level, read from `inputs`).
using GeneratedClockCommitFn = void (*)(
    std::span<const NativeWordSlot> inputs,
    std::span<NativeWordSlot> storage);

// Applies FlopsInitial to every flop Q leaf in the design, in generated
// (flop-declaration) order. When use_initial_values is true, flops with a
// declaration initializer take that value instead of the mode.
using GeneratedFlopsInitFn = void (*)(
    std::span<NativeWordSlot> storage,
    MateFlopsInitial mode,
    bool use_initial_values,
    std::mt19937_64& rng);

struct GeneratedModelMetadata {
    std::vector<GeneratedInputMetadata> inputs;
    std::vector<GeneratedOutputMetadata> outputs;
    std::vector<GeneratedClockMetadata> clocks;
    std::vector<GeneratedResetMetadata> resets;
    std::vector<GeneratedStorageMetadata> storage;
    // Every observable name, aliases included; see GeneratedObservableMetadata.
    std::vector<GeneratedObservableMetadata> observable_names;
    GeneratedCombinationalEvaluateFn evaluate_combinational = nullptr;
    // Word count of the contiguous scratch buffer for values crossing
    // generated combinational chunk boundaries. The generated code knows each
    // value's offset/width statically; the runtime only allocates the buffer.
    // Not part of runtime-observable metadata.
    size_t spill_words_count = 0;
    // One entry per `resets`/`clocks` entry, same order, when
    // `evaluate_combinational` is set. Empty when the model has no native
    // combinational evaluator (interpreter-only fallback).
    std::vector<GeneratedResetApplyFn> reset_apply;
    std::vector<GeneratedClockCommitFn> clock_commit;
    // Set alongside evaluate_combinational for native models.
    GeneratedFlopsInitFn flops_init = nullptr;
};

} // namespace mate::abi
