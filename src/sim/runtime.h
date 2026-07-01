#pragma once

#include "mateir/mateir.h"
#include "sim/runtime_metadata.h"
#include "sim/sim_value.h"

#include <functional>
#include <map>
#include <optional>
#include <random>
#include <set>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace mate {

enum class FlopsInitial { Random, AllZeros, AllOnes };

struct CollectedFlop {
    const FlopInfo* flop;
    ClockId clock_domain;
    ResetDomains reset_domains;
};

struct RuntimeInputUpdate {
    RuntimeInputId input;
    SimValue value;
};

// MateIR-backed interpreted runtime for one mutable DUT instance.
class MateIRRuntime {
public:
    MateIRRuntime(const std::string& name,
                  const Module& mod,
                  const MateIR& ir,
                  const MateIRRuntimeMetadata& metadata);

    const MateIRRuntimeMetadata& metadata() const { return metadata_; }

    void initialize(FlopsInitial mode, std::mt19937_64& rng);
    void initializeInputsAndEvaluate(std::span<const RuntimeInputUpdate> async_inputs,
                                     std::span<const RuntimeInputUpdate> sync_inputs);
    void setInputValues(std::span<const RuntimeInputUpdate> inputs);
    void applyClockEdge(ClockId clock,
                        edge_t edge,
                        std::span<const RuntimeInputUpdate> inputs_before_edge);
    void applyResetEdge(ResetId reset, edge_t edge);
    void applyAsyncSignalEdge(RuntimeInputId input, const SimValue& value);

    // Stable observability surface: outputs and observables are read only
    // through handles. Adapters (file simulator, future DPI/cocotb) must use
    // these and never reach into runtime/DFG state directly.
    SimValue getOutput(RuntimeOutputId output) const;
    SimValue getObservable(RuntimeObservableId observable) const;

    // DFG-node trace/debug hooks.
    //
    // These are a deliberately narrow, node-level introspection surface used by
    // the optional `--trace-dfg-*` JSONL emitter. They are NOT the observable
    // API and NOT part of the future foreign ABI: the DFG trace reports raw
    // evaluation nodes (CONST/X/internal arithmetic) that have no observable
    // handle, so they cannot be expressed through getObservable. `trace_sink`
    // is invoked by the interpreted evaluator with per-node inputs/results.
    const SimValue& checkedGetRef(const DFGNode* node, const DFGNode* context = nullptr) const;
    SimValue checkedGet(const DFGNode* node, const DFGNode* context = nullptr) const;
    const SimValue* findNodeValue(const DFGNode* node) const;
    bool isFlopQNode(const DFGNode* node) const;
    std::optional<size_t> nodeIndex(const DFGNode* node) const;

    std::function<void(const DFGNode*,
                       const std::vector<std::pair<std::string, SimValue>>&,
                       const SimValue&,
                       const std::string&)> trace_sink;

private:
    std::string instance_name_;
    const Module& module_def_;
    const MateIR& ir_;
    const MateIRRuntimeMetadata& metadata_;

    std::map<const DFGNode*, SimValue> values_;
    std::map<std::string, SimValue> async_values_;

    std::vector<const DFGNode*> topo_order_;
    std::unordered_map<const DFGNode*, size_t> node_indices_;
    std::map<const DFGNode*, const FlopInfo*> flop_q_nodes_;
    std::map<ClockId, std::vector<CollectedFlop>> flops_by_clock_;
    std::map<ResetId, std::vector<CollectedFlop>> flops_by_reset_;
    std::map<std::string, std::vector<ClockId>> clock_domains_by_top_input_;
    std::map<std::string, std::vector<ResetId>> reset_domains_by_top_input_;
    std::map<ResetId, std::string> reset_top_input_by_id_;

    void buildTopInputDomainMaps();
    void buildTopology();
    void buildFlopMaps();
    void initConsts();
    void initXs(std::mt19937_64& rng);
    void initFlops(FlopsInitial mode, std::mt19937_64& rng);
    void initializeAsyncInput(RuntimeInputId input, const SimValue& value);
    void setSyncInput(RuntimeInputId input, const SimValue& value);
    void setInputValue(RuntimeInputId input, const SimValue& value);
    void updateOutputs();
    std::set<ResetId> activeResetDomains() const;
    void validateSyncUpdateForClock(ClockId clock_id, const RuntimeInputUpdate& update) const;
    RuntimeInputId clockSourceInput(ClockId clock_id) const;
    RuntimeInputId resetSourceInput(ResetId reset_id) const;
    void validateClockEdge(ClockId clock_id, edge_t edge) const;
    void validateResetEdge(ResetId reset_id, edge_t edge) const;
    SimValue sourceValueForEdge(RuntimeInputId input, edge_t edge) const;
    void setTopInputValue(const std::string& leaf_name, const SimValue& value);
    void setAsyncInputValue(const std::string& leaf_name, const SimValue& value);
    const SimValue& asyncInputValue(const std::string& leaf_name) const;
    bool resetDomainActive(ResetId id) const;
    void applyResetDomain(ResetId reset_id);
    void applyClockDomain(ClockId clock_id);
    void evaluateCombinational();
    void assignFlopResetLeaves(const FlopInfo& flop, int64_t reset_value);
    void copyFlopDToQLeaves(const FlopInfo& flop);
    SimValue evaluateNode(const DFGNode* node);

    static SimValue maskToWidth(const SimValue& val, const DFGNode* node);
};

} // namespace mate
