#pragma once

#include "ir/resolved.h"
#include "vcd_tracer.hpp"

#include <map>
#include <memory>
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

class Simulator {
public:
    Simulator(const ResolvedModule& module, const SimConfig& config);
    void run();

private:
    const ResolvedModule& module_;
    const SimConfig& config_;

    // Topological order for combinational evaluation
    std::vector<const DFGNode*> topo_order_;

    // Value storage: node pointer -> value
    std::map<const DFGNode*, int64_t> values_;

    // Async event timeline (merged, time-sorted)
    std::vector<AsyncEvent> timeline_;

    // Flop lookup maps
    std::map<std::string, std::vector<const FlopInfo*>> flops_by_clock_;
    std::map<std::string, std::vector<const FlopInfo*>> flops_by_reset_;

    // Flop .q node -> FlopInfo mapping for quick access
    std::map<const DFGNode*, const FlopInfo*> flop_q_nodes_;

    // Which inputs are async (clock/reset) vs sync
    std::set<std::string> async_inputs_;
    std::map<std::string, std::vector<int64_t>> sync_input_data_;  // name -> values from file
    std::map<std::string, size_t> sync_input_pos_;  // name -> current position

    // Output recording
    std::map<std::string, std::vector<int64_t>> recorded_values_;

    // VCD tracing (hierarchical: inputs/signals/outputs sub-scopes)
    std::unique_ptr<vcd_tracer::top> vcd_top_;
    std::map<const DFGNode*, std::unique_ptr<vcd_tracer::value<int64_t>>> vcd_values_;
    std::map<std::string, std::unique_ptr<vcd_tracer::value<int64_t>>> vcd_async_values_;
    std::vector<std::unique_ptr<vcd_tracer::value<int64_t>>> vcd_params_;

    // VCD tracing (flat: all signals at root level, for comparison with Verilator)
    std::unique_ptr<vcd_tracer::top> vcd_flat_top_;
    std::map<const DFGNode*, std::unique_ptr<vcd_tracer::value<int64_t>>> vcd_flat_values_;
    std::map<std::string, std::unique_ptr<vcd_tracer::value<int64_t>>> vcd_flat_async_values_;
    std::vector<std::unique_ptr<vcd_tracer::value<int64_t>>> vcd_flat_params_;

    void setupVcd(std::ofstream& vcd_out);
    void setupVcdFlat(std::ofstream& vcd_out);
    void updateVcdValues(std::ofstream& vcd_out, int64_t time_ns,
                         const std::map<std::string, int64_t>& async_values);
    void updateVcdValuesFlat(std::ofstream& vcd_out, int64_t time_ns,
                             const std::map<std::string, int64_t>& async_values);

    void buildTopology();
    void buildTimeline();
    void buildFlopMaps();
    void loadSyncInputs();

    void evaluateCombinational();
    int64_t evaluateNode(const DFGNode* node);
    int64_t maskToWidth(int64_t val, const DFGNode* node);

    void advanceSyncInputs();
    void recordOutputs();
    void writeOutputFiles();
};

} // namespace custom_hdl
