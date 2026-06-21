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

struct ActiveDomainEdges {
    std::set<ClockId> clocks;
    std::set<ResetId> resets;
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
    void initializeAsyncInput(RuntimeInputId input, const SimValue& value);
    ActiveDomainEdges setAsyncInput(RuntimeInputId input, const SimValue& value);
    void setSyncInput(RuntimeInputId input, const SimValue& value);
    void clockEdge(ClockId clock_id, std::span<const RuntimeInputUpdate> sync_inputs = {});
    void resetEdge(ResetId reset_id);
    void updateOutputs(int64_t time_ns);

    SimValue getOutput(RuntimeOutputId output) const;
    SimValue getObservable(RuntimeObservableId observable) const;

    std::set<ResetId> activeResetDomains() const;

    // Compatibility/debug accessors for the existing file harness, trace, and VCD layers.
    void setTopInputValue(const std::string& leaf_name, const SimValue& value);
    void setAsyncInputValue(const std::string& leaf_name, const SimValue& value);
    const SimValue& asyncInputValue(const std::string& leaf_name) const;
    ActiveDomainEdges detectActiveDomainEdges(const std::string& leaf_name,
                                              const SimValue& old_value,
                                              const SimValue& new_value) const;
    bool resetDomainActive(ResetId id) const;
    void applyResetEdge(ResetId reset_id);
    void applyClockEdge(ClockId clock_id);
    void evaluateCombinational();
    const SimValue& checkedGetRef(const DFGNode* node, const DFGNode* context = nullptr) const;
    SimValue checkedGet(const DFGNode* node, const DFGNode* context = nullptr) const;
    const SimValue* findNodeValue(const DFGNode* node) const;
    const SimValue* findAsyncInputValue(const std::string& leaf_name) const;
    bool isFlopQNode(const DFGNode* node) const;
    std::optional<size_t> nodeIndex(const DFGNode* node) const;

    int64_t current_time_ns = 0;
    std::function<void(int64_t,
                       const DFGNode*,
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
    void assignFlopResetLeaves(const FlopInfo& flop, int64_t reset_value);
    void copyFlopDToQLeaves(const FlopInfo& flop);
    SimValue evaluateNode(const DFGNode* node);

    static SimValue maskToWidth(const SimValue& val, const DFGNode* node);
};

} // namespace mate
