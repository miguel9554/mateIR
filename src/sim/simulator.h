#pragma once

#include "ir/resolved.h"
#include "vcd_tracer.hpp"

#include <map>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <unordered_set>
#include <vector>

namespace custom_hdl {

enum class FlopsInitial { Random, AllZeros, AllOnes };

struct DebugNodeSpec {
    std::string module_path; // empty = match any module; otherwise suffix-matched against hierarchy path
    std::string node_name;
};

struct SimConfig {
    std::vector<std::string> source_files;
    std::string top_module;
    std::string inputs_dir;
    std::string output_dir;
    std::map<std::string, int64_t> parameters;
    std::vector<DebugNodeSpec> debug_dfg_nodes;
    FlopsInitial flops_initial = FlopsInitial::Random;
    std::optional<uint64_t> flops_initial_seed;
};

struct AsyncEvent {
    int64_t time;
    std::string signal_name;
    int64_t value;
};

// Runtime state for the flat (inlined) design.
// After DFG inlining, there is a single ModuleInstance for the entire design.
// All nodes from all submodules are in module_def.dfg (the flat top DFG).
struct ModuleInstance {
    std::string instance_name;
    const ResolvedModule& module_def;

    // Runtime values for all DFG nodes (flat — covers entire design hierarchy)
    std::map<const DFGNode*, int64_t> values;

    // Async signal state for edge detection (port_name -> current value)
    std::map<std::string, int64_t> async_values;

    // Flat topology and flop maps (cover all submodules after inlining)
    std::vector<const DFGNode*> topo_order;
    std::map<const DFGNode*, const FlopInfo*> flop_q_nodes;
    std::map<std::string, std::vector<const FlopInfo*>> flops_by_clock;
    std::map<std::string, std::vector<const FlopInfo*>> flops_by_reset;
    std::set<std::string> async_input_names;  // input ports used as clock/reset

    // VCD tracking: sync signals keyed by DFG node, async signals keyed by name.
    // Vectors because the same DFG node may appear in multiple scopes (e.g. a top-level
    // input and all submodule inputs that were inlined to point to the same driver node).
    std::map<const DFGNode*, std::vector<std::unique_ptr<vcd_tracer::value<int64_t>>>> vcd_values;
    std::map<const DFGNode*, std::vector<std::unique_ptr<vcd_tracer::value<int64_t>>>> vcd_flat_values;
    std::map<std::string, std::vector<std::unique_ptr<vcd_tracer::value<int64_t>>>> vcd_async_values;
    std::map<std::string, std::vector<std::unique_ptr<vcd_tracer::value<int64_t>>>> vcd_flat_async_values;

    ModuleInstance(const std::string& name, const ResolvedModule& mod);

    // Construction helpers
    void buildTopology();
    void buildFlopMaps();  // recurses over hierarchyInstantiation
    void initConsts();
    void initFlops(FlopsInitial mode, std::mt19937_64& rng);

    // Stateful: processes an async event, detects edges, does d->q / reset
    void setAsyncEvent(const std::string& signalName, int64_t newValue);

    // Single-pass combinational evaluation (no fixpoint — flat DAG has no cycles)
    void evaluateCombinational();

    // Evaluate a single node
    int64_t evaluateNode(const DFGNode* node);

    // Mask value to node's bit width
    static int64_t maskToWidth(int64_t val, const DFGNode* node);

    // values.at() with a useful error message naming the missing node
    int64_t checkedGet(const DFGNode* node, const DFGNode* context = nullptr) const;
};

class Simulator {
public:
    Simulator(const ResolvedModule& module, const SimConfig& config);
    void run();

private:
    const ResolvedModule& module_;
    const SimConfig& config_;

    // The root module instance (contains all state and logic)
    std::unique_ptr<ModuleInstance> root_;

    // Testbench infrastructure
    std::vector<AsyncEvent> timeline_;
    std::set<std::string> async_inputs_;
    std::set<std::string> clock_inputs_;  // inputs with type Clock
    // Clock name -> active edge (from sync input ResolvedSignals)
    std::map<std::string, edge_t> clock_active_edge_;
    // Sync input -> clock name
    std::map<std::string, std::string> sync_input_clock_;
    std::map<std::string, std::vector<int64_t>> sync_input_data_;
    std::map<std::string, size_t> sync_input_pos_;
    std::map<std::string, std::vector<int64_t>> recorded_values_;

    // VCD (top-level files, scopes built recursively from root_)
    std::unique_ptr<vcd_tracer::top> vcd_top_;
    std::unique_ptr<vcd_tracer::top> vcd_flat_top_;
    std::vector<std::unique_ptr<vcd_tracer::value<int64_t>>> vcd_params_;
    std::vector<std::unique_ptr<vcd_tracer::value<int64_t>>> vcd_flat_params_;

    // Testbench methods
    void buildTimeline();
    void loadSyncInputs();
    void advanceSyncInputs(const std::set<std::string>& active_clocks);
    void recordOutputs();
    void writeOutputFiles();
    void setupVcd(std::ofstream& vcd_out);
    void setupVcdFlat(std::ofstream& vcd_out);
    void setupVcdHierForModule(const ResolvedModule& mod, vcd_tracer::module& scope,
                               const std::unordered_set<const DFGNode*>& alive);
    void setupVcdFlatForModule(const ResolvedModule& mod, vcd_tracer::module& scope,
                               const std::unordered_set<const DFGNode*>& alive);
    void updateVcdValues(std::ofstream& vcd_out, int64_t time_ns);
    void updateVcdValuesFlat(std::ofstream& vcd_out, int64_t time_ns);
    void updateVcdForRoot(bool flat);
};

} // namespace custom_hdl
