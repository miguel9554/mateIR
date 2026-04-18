#pragma once

#include "mateir/module.h"
#include "mateir/debug.h"
#include "sim/sim_value.h"
#include "sim/vcd_writer.h"

#include <map>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <unordered_set>
#include <vector>

namespace custom_hdl {

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
};

struct AsyncEvent {
    int64_t time;
    std::string signal_name;
    SimValue value;
};

struct CollectedFlop {
    const FlopInfo* flop;
    std::string clock_name;
    std::optional<std::string> reset_name;
};

// Runtime state for the flat (inlined) design.
// After DFG inlining, there is a single ModuleInstance for the entire design.
// All nodes from all submodules are in module_def.dfg (the flat top DFG).
struct ModuleInstance {
    std::string instance_name;
    const Module& module_def;

    // Runtime values for all DFG nodes (flat — covers entire design hierarchy)
    std::map<const DFGNode*, SimValue> values;

    // Async signal state for edge detection (port_name -> current value)
    std::map<std::string, SimValue> async_values;

    // Flat topology and flop maps (cover all submodules after inlining)
    std::vector<const DFGNode*> topo_order;
    std::map<const DFGNode*, const FlopInfo*> flop_q_nodes;
    std::map<std::string, std::vector<CollectedFlop>> flops_by_clock;
    std::map<std::string, std::vector<CollectedFlop>> flops_by_reset;
    std::set<std::string> async_input_names;  // input ports used as clock/reset

    ModuleInstance(const std::string& name, const Module& mod);

    // Construction helpers
    void buildTopology();
    void buildFlopMaps();  // recurses over hierarchyInstantiation
    void initConsts();
    void initFlops(FlopsInitial mode, std::mt19937_64& rng);

    // Single-pass combinational evaluation (no fixpoint — flat DAG has no cycles)
    void evaluateCombinational();

    // Evaluate a single node
    SimValue evaluateNode(const DFGNode* node);

    // Mask value to node's bit width
    static SimValue maskToWidth(const SimValue& val, const DFGNode* node);

    // values.at() with a useful error message naming the missing node
    SimValue checkedGet(const DFGNode* node, const DFGNode* context = nullptr) const;
};

class Simulator {
public:
    Simulator(const Module& module, const SimConfig& config);
    void run();

private:
    const Module& module_;
    const SimConfig& config_;

    // The root module instance (contains all state and logic)
    std::unique_ptr<ModuleInstance> root_;

    // Testbench infrastructure
    std::vector<AsyncEvent> timeline_;
    std::set<std::string> async_inputs_;
    // Sync input -> clock name
    std::map<std::string, std::string> sync_input_clock_;
    std::map<std::string, std::vector<SimValue>> sync_input_data_;
    std::map<std::string, size_t> sync_input_pos_;
    std::map<std::string, std::vector<SimValue>> recorded_values_;

    std::unique_ptr<VcdWriter> vcd_;

    // Testbench methods
    void buildTimeline();
    void loadSyncInputs();
    void advanceSyncInputs(const std::set<std::string>& active_clocks);
    void recordOutputs();
    void writeOutputFiles();
};

} // namespace custom_hdl
