#pragma once

#include "mateir/mateir.h"
#include "sim/sim_value.h"

#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace mate {

struct RuntimeInputId {
    size_t value = 0;
    auto operator<=>(const RuntimeInputId&) const = default;
};

struct RuntimeOutputId {
    size_t value = 0;
    auto operator<=>(const RuntimeOutputId&) const = default;
};

struct RuntimeClockId {
    size_t value = 0;
    auto operator<=>(const RuntimeClockId&) const = default;
};

struct RuntimeResetId {
    size_t value = 0;
    auto operator<=>(const RuntimeResetId&) const = default;
};

struct RuntimeFlopId {
    size_t value = 0;
    auto operator<=>(const RuntimeFlopId&) const = default;
};

struct RuntimeObservableId {
    size_t value = 0;
    auto operator<=>(const RuntimeObservableId&) const = default;
};

enum class RuntimeInputKind {
    Async,
    Sync,
    Clock,
    Reset,
};

enum class RuntimeObservableKind {
    Input,
    Output,
    Internal,
    FlopD,
    FlopQ,
};

enum class FlopsInitial {
    Random,
    AllZeros,
    AllOnes,
};

struct RuntimeInputUpdate {
    RuntimeInputId input;
    SimValue value;
};

struct RuntimeInputLeafMetadata {
    RuntimeInputId id;
    std::string port_name;
    std::string leaf_name;
    size_t leaf_index = 0;
    Type type;
    const DFGNode* node = nullptr;
    RuntimeInputKind kind = RuntimeInputKind::Async;
    std::optional<ClockId> clock_domain;
    std::optional<ResetId> reset_domain;
};

struct RuntimeOutputLeafMetadata {
    RuntimeOutputId id;
    std::string port_name;
    std::string leaf_name;
    size_t leaf_index = 0;
    Type type;
    const DFGNode* node = nullptr;
};

struct RuntimeClockMetadata {
    RuntimeClockId id;
    ClockId domain_id = InvalidClockId;
    std::string display_name;
    edge_t edge = POSEDGE;
    RuntimeInputId source_input;
};

struct RuntimeResetMetadata {
    RuntimeResetId id;
    ResetId domain_id = InvalidResetId;
    std::string display_name;
    edge_t active_edge = POSEDGE;
    RuntimeInputId source_input;
};

struct RuntimeFlopLeafMetadata {
    std::string leaf_name;
    std::string d_leaf_name;
    std::string q_leaf_name;
    size_t leaf_index = 0;
    Type type;
    const DFGNode* d_node = nullptr;
    const DFGNode* q_node = nullptr;
    // Declaration initializer value, applied at time 0 when the runtime
    // enables initial values (instead of the FlopsInitial policy).
    std::optional<int64_t> initial_value;
};

struct RuntimeFlopMetadata {
    RuntimeFlopId id;
    const FlopInfo* flop = nullptr;
    std::string name;
    Type type;
    ClockId clock_domain = InvalidClockId;
    ResetDomains reset_domains;
    std::optional<int64_t> reset_value;
    std::vector<RuntimeFlopLeafMetadata> leaves;
};

struct RuntimeObservableMetadata {
    RuntimeObservableId id;
    RuntimeObservableKind kind = RuntimeObservableKind::Internal;
    std::string full_path;
    std::string leaf_name;
    Type type;
    const DFGNode* node = nullptr;
    std::optional<RuntimeInputId> input;
    std::optional<RuntimeOutputId> output;
    std::optional<RuntimeFlopId> flop;
    size_t leaf_index = 0;
};

struct MateIRRuntimeMetadata {
    std::vector<RuntimeInputLeafMetadata> input_leaves;
    std::vector<RuntimeOutputLeafMetadata> output_leaves;
    std::vector<RuntimeClockMetadata> clocks;
    std::vector<RuntimeResetMetadata> resets;
    std::vector<RuntimeFlopMetadata> flops;
    std::vector<RuntimeObservableMetadata> observables;

    std::unordered_map<std::string, RuntimeInputId> input_by_leaf_name;
    std::unordered_map<std::string, RuntimeOutputId> output_by_leaf_name;
    std::unordered_map<std::string, RuntimeObservableId> observable_by_full_path;
    std::unordered_map<std::string, RuntimeObservableId> observable_by_unique_leaf_name;
    std::unordered_set<std::string> ambiguous_observable_leaf_names;
    std::map<const DFGNode*, std::vector<RuntimeObservableId>> observables_by_node;
    std::map<std::string, std::vector<ClockId>> clock_domains_by_top_input;
    std::map<std::string, std::vector<ResetId>> reset_domains_by_top_input;
    std::map<ResetId, std::string> reset_top_input_by_id;
    std::map<ClockId, std::vector<RuntimeFlopId>> flops_by_clock;
    std::map<ResetId, std::vector<RuntimeFlopId>> flops_by_reset;

    const RuntimeInputLeafMetadata* findInput(std::string_view leaf_name) const;
    const RuntimeOutputLeafMetadata* findOutput(std::string_view leaf_name) const;
};

MateIRRuntimeMetadata buildMateIRRuntimeMetadata(const MateIR& ir);

} // namespace mate
