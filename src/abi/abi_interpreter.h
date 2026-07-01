#pragma once

#include "abi/mate_model_abi.h"
#include "frontends/frontend.h"
#include "sim/sim_value.h"

#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace mate::abi {

struct InterpreterModelConfig {
    std::string top_module;
    std::vector<std::string> source_files;
    std::vector<std::string> domain_files;
    std::map<std::string, int64_t> parameters;
    TopDomainMode top_domain_mode = TopDomainMode::Yaml;
};

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
};

MateStatusCode createInterpreterModel(const InterpreterModelConfig& config,
                                      const GeneratedModelMetadata& generated_metadata,
                                      const MateModel** out_model,
                                      MateStatus* status);

MateStatusCode createInterpreterModel(const InterpreterModelConfig& config,
                                      const MateModel** out_model,
                                      MateStatus* status);

} // namespace mate::abi
