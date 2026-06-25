#pragma once

#include "mateir/mateir.h"
#include "mateir/debug.h"
#include "sim/runtime_model.h"
#include "sim/sim_value.h"
#include "sim/vcd_writer.h"

#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_set>
#include <vector>

namespace mate {

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

class Simulator {
public:
    Simulator(const RtlRuntimeModel& model, const SimConfig& config);
    void run();

private:
    const RtlRuntimeModel& model_;
    const MateIR& ir_;
    const Module& module_;
    const SimConfig& config_;
    const MateIRRuntimeMetadata& runtime_metadata_;

    std::unique_ptr<RtlRuntimeInstance> runtime_;

    // Testbench infrastructure
    std::vector<AsyncEvent> timeline_;
    std::set<std::string> async_inputs_;
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
    int64_t runtime_trace_time_ns_ = 0;

    // Testbench methods
    void buildTimeline();
    void loadSyncInputs();
    std::vector<RuntimeInputUpdate> collectPostClockSyncInputs(ClockId active_clock);
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
