#pragma once

#include "mateir/mateir.h"
#include "mateir/debug.h"
#include "sim/sim_value.h"
#include "sim/vcd_writer.h"

#include <functional>
#include <fstream>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace mate {

enum class FlopsInitial { Random, AllZeros, AllOnes };

struct SimConfig {
    std::vector<std::string> source_files;
    std::string top_module;
    std::string inputs_dir;
    std::string output_dir;
    std::map<std::string, int64_t> parameters;
    std::vector<DebugNodeSpec> debug_dfg_nodes;
    FlopsInitial flops_initial = FlopsInitial::Random;
    std::optional<uint64_t> flops_initial_seed;
    std::vector<std::string> trace_dfg_nodes;
    std::vector<std::string> trace_dfg_cones;
    std::vector<std::string> trace_dfg_ops;
};

struct AsyncEvent {
    int64_t time;
    std::string signal_name;
    SimValue value;
};

struct CollectedFlop {
    const FlopInfo* flop;
    ClockId clock_domain;
    ResetDomains reset_domains;
};

// Runtime state for the flat (inlined) design.
// After DFG inlining, there is a single ModuleInstance for the entire design.
// All nodes from all submodules are in module_def.dfg (the flat top DFG).
struct ModuleInstance {
    std::string instance_name;
    const Module& module_def;
    const MateIR& ir;

    // Runtime values for all DFG nodes (flat — covers entire design hierarchy)
    std::map<const DFGNode*, SimValue> values;

    // Async signal state for edge detection (port_name -> current value)
    std::map<std::string, SimValue> async_values;

    // Flat topology and flop maps (cover all submodules after inlining)
    std::vector<const DFGNode*> topo_order;
    std::unordered_map<const DFGNode*, size_t> node_indices;
    std::map<const DFGNode*, const FlopInfo*> flop_q_nodes;
    std::map<ClockId, std::vector<CollectedFlop>> flops_by_clock;
    std::map<ResetId, std::vector<CollectedFlop>> flops_by_reset;
    int64_t current_time_ns = 0;
    std::function<void(int64_t,
                       const DFGNode*,
                       const std::vector<std::pair<std::string, SimValue>>&,
                       const SimValue&,
                       const std::string&)> trace_sink;

    ModuleInstance(const std::string& name, const Module& mod, const MateIR& ir);

    // Construction helpers
    void buildTopology();
    void buildFlopMaps();  // recurses over hierarchyInstantiation
    void initConsts();
    void initXs(std::mt19937_64& rng);
    void initFlops(FlopsInitial mode, std::mt19937_64& rng);

    // Single-pass combinational evaluation (no fixpoint — flat DAG has no cycles)
    void evaluateCombinational();

    // Evaluate a single node
    SimValue evaluateNode(const DFGNode* node);

    // Mask value to node's bit width
    static SimValue maskToWidth(const SimValue& val, const DFGNode* node);

    // values.at() with a useful error message naming the missing node
    const SimValue& checkedGetRef(const DFGNode* node, const DFGNode* context = nullptr) const;
    SimValue checkedGet(const DFGNode* node, const DFGNode* context = nullptr) const;
};

class Simulator {
public:
    Simulator(const MateIR& ir, const SimConfig& config);
    void run();

private:
    const MateIR& ir_;
    const Module& module_;
    const SimConfig& config_;

    // The root module instance (contains all state and logic)
    std::unique_ptr<ModuleInstance> root_;

    // Testbench infrastructure
    std::vector<AsyncEvent> timeline_;
    std::set<std::string> async_inputs_;
    std::map<std::string, std::vector<ClockId>> clock_domains_by_top_input_;
    std::map<std::string, std::vector<ResetId>> reset_domains_by_top_input_;
    std::map<ResetId, std::string> reset_top_input_by_id_;
    // Sync input -> clock domain
    std::map<std::string, ClockId> sync_input_clock_;
    std::map<std::string, std::vector<SimValue>> sync_input_data_;
    std::map<std::string, size_t> sync_input_pos_;
    std::map<std::string, std::vector<SimValue>> recorded_values_;

    std::unique_ptr<VcdWriter> vcd_;
    std::unique_ptr<std::ofstream> dfg_trace_out_;
    std::unordered_set<const DFGNode*> traced_nodes_;
    std::set<std::string> traced_ops_;
    std::optional<std::string> dfg_trace_path_;

    // Testbench methods
    void buildTopInputDomainMaps();
    void buildTimeline();
    void loadSyncInputs();
    void advanceSyncInputs(const std::set<ClockId>& active_clocks);
    bool resetDomainActive(ResetId id) const;
    std::set<ResetId> activeResetDomains() const;
    void recordOutputs();
    void writeOutputFiles();
    void initTraceConfiguration();
    bool shouldTraceNode(const DFGNode* node) const;
    void emitPassiveTraceEvents(int64_t time_ns);
    void emitTraceEvent(int64_t time_ns,
                        const DFGNode* node,
                        const std::vector<std::pair<std::string, SimValue>>& inputs,
                        const SimValue& result,
                        const std::string& decisions_json = "");
};

} // namespace mate
