#pragma once

#include "ir/resolved.h"
#include "vcd_tracer.hpp"

#include <map>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <vector>

namespace custom_hdl {

enum class FlopsInitial { Random, AllZeros, AllOnes };

struct SimConfig {
    std::vector<std::string> source_files;
    std::string top_module;
    std::string inputs_dir;
    std::string output_dir;
    std::map<std::string, int64_t> parameters;
    std::vector<std::string> debug_dfg_nodes;
    FlopsInitial flops_initial = FlopsInitial::Random;
    std::optional<uint64_t> flops_initial_seed;
};

struct AsyncEvent {
    int64_t time;
    std::string signal_name;
    int64_t value;
};

// A single module instance in the hierarchy.
// Holds per-instance runtime state and all evaluation logic.
struct ModuleInstance {
    std::string instance_name;
    const ResolvedModule& module_def;

    // Per-instance runtime state
    std::map<const DFGNode*, int64_t> values;
    std::map<std::pair<const DFGNode*, int>, int64_t> module_output_values;

    // Async signal state for edge detection (port_name -> current value)
    std::map<std::string, int64_t> async_values;

    // Per-instance topology and flop maps
    std::vector<const DFGNode*> topo_order;
    std::map<const DFGNode*, const FlopInfo*> flop_q_nodes;
    std::map<std::string, std::vector<const FlopInfo*>> flops_by_clock;
    std::map<std::string, std::vector<const FlopInfo*>> flops_by_reset;
    std::set<std::string> async_input_names;  // input ports used as clock/reset

    // Children: MODULE DFG node pointer -> child instance
    std::map<const DFGNode*, std::unique_ptr<ModuleInstance>> children;

    // VCD tracking
    std::map<const DFGNode*, std::unique_ptr<vcd_tracer::value<int64_t>>> vcd_values;
    std::map<const DFGNode*, std::unique_ptr<vcd_tracer::value<int64_t>>> vcd_flat_values;

    ModuleInstance(const std::string& name, const ResolvedModule& mod);

    // Construction helpers
    void buildTopology();
    void buildFlopMaps();
    void initConsts();
    void initFlops(FlopsInitial mode, std::mt19937_64& rng);

    // Stateful: processes an async event, detects edges, does d->q / reset
    void setAsyncEvent(const std::string& signalName, int64_t newValue);

    // Pure combo evaluation with fixpoint (recurses into children)
    void evaluateCombinational();

    // Evaluate a single non-MODULE node
    int64_t evaluateNode(const DFGNode* node);

    // Evaluate a MODULE node: push inputs, setAsyncEvent if needed,
    // call child.evaluateCombinational(), pull outputs. Returns true if inputs changed.
    bool evaluateModuleNode(const DFGNode* moduleNode);

    // Mask value to node's bit width
    static int64_t maskToWidth(int64_t val, const DFGNode* node);

    // Get input value, handling MODULE multi-output sources
    int64_t getInputValue(const DFGOutput& input) const;
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
    std::map<std::string, std::unique_ptr<vcd_tracer::value<int64_t>>> vcd_async_values_;
    std::map<std::string, std::unique_ptr<vcd_tracer::value<int64_t>>> vcd_flat_async_values_;
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
    void setupVcdHierForInstance(ModuleInstance& inst, vcd_tracer::module& parent);
    void setupVcdFlatForInstance(ModuleInstance& inst, vcd_tracer::module& parent);
    void updateVcdValues(std::ofstream& vcd_out, int64_t time_ns,
                         const std::map<std::string, int64_t>& async_values);
    void updateVcdValuesFlat(std::ofstream& vcd_out, int64_t time_ns,
                              const std::map<std::string, int64_t>& async_values);
    void updateVcdForInstance(ModuleInstance& inst);
};

} // namespace custom_hdl
