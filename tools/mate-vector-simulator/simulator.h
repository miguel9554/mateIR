#pragma once

#include "mateir/mateir.h"
#include "sim_engine.h"
#include "sim/sim_value.h"
#include "vcd_writer.h"

#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace mate {

struct SimConfig {
    std::vector<std::string> source_files;
    std::string top_module;
    std::string inputs_dir;
    std::string output_dir;
    std::map<std::string, int64_t> parameters;
    FlopsInitial flops_initial = FlopsInitial::Random;
    std::optional<uint64_t> flops_initial_seed;
};

struct AsyncEvent {
    int64_t time;
    std::string signal_name;
    SimValue value;
};

struct SyncInputTransition {
    std::vector<RuntimeInputUpdate> before_edge;
    std::vector<RuntimeInputUpdate> after_edge;
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

    std::unique_ptr<SimEngine> runtime_;

    // Testbench infrastructure
    std::vector<AsyncEvent> timeline_;
    std::set<std::string> async_inputs_;
    // Sync input -> clock domain
    std::map<std::string, ClockId> sync_input_clock_;
    std::map<std::string, std::vector<SimValue>> sync_input_data_;
    std::map<std::string, size_t> sync_input_pos_;
    std::map<std::string, SimValue> async_input_values_;
    std::map<std::string, std::vector<SimValue>> recorded_values_;

    std::unique_ptr<VcdWriter> vcd_;

    // Testbench methods
    void buildTimeline();
    void loadSyncInputs();
    SyncInputTransition collectClockSyncInputTransition(ClockId active_clock);
    std::optional<edge_t> updateAsyncInputAndDetectEdge(const RuntimeInputUpdate& update,
                                                        int64_t time_ns);
    bool isClockOrResetSource(const std::string& leaf_name) const;
    void recordOutputs();
    void writeOutputFiles();
};

} // namespace mate
