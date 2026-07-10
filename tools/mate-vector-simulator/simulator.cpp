#include "simulator.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <format>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <optional>
#include <random>
#include <set>
#include <span>
#include <sstream>
#include <stdexcept>
#include <utility>


namespace mate {

namespace {

} // namespace

// Parse a time token like "5ns" or "1.5us" into integer nanoseconds.
// ============================================================================

static int64_t parseTimeWithUnit(const std::string& token,
                                 const std::string& file_path,
                                 const std::string& line) {
    size_t unit_start = token.size();
    while (unit_start > 0 && std::isalpha(static_cast<unsigned char>(token[unit_start - 1]))) {
        --unit_start;
    }

    if (unit_start == 0 || unit_start == token.size()) {
        throw CompilerError(std::format(
            "Simulator: bad time token '{}' in '{}' (line: {})"
            " — expected <number><unit> like 5ns or 1.5us",
            token, file_path, line));
    }

    std::string num_str = token.substr(0, unit_start);
    std::string unit    = token.substr(unit_start);

    double value;
    try {
        size_t pos;
        value = std::stod(num_str, &pos);
        if (pos != num_str.size()) throw std::invalid_argument("trailing chars");
    } catch (...) {
        throw CompilerError(std::format(
            "Simulator: cannot parse number '{}' in time token '{}' (file '{}', line: {})",
            num_str, token, file_path, line));
    }

    double multiplier;
    if      (unit == "s")  multiplier = 1e9;
    else if (unit == "ms") multiplier = 1e6;
    else if (unit == "us") multiplier = 1e3;
    else if (unit == "ns") multiplier = 1.0;
    else if (unit == "ps") multiplier = 1e-3;
    else if (unit == "fs") multiplier = 1e-6;
    else {
        throw CompilerError(std::format(
            "Simulator: unknown time unit '{}' in token '{}' (file '{}', line: {})"
            " — supported: s, ms, us, ns, ps, fs",
            unit, token, file_path, line));
    }

    return static_cast<int64_t>(std::round(value * multiplier));
}

// ============================================================================
// Build async event timeline from clock/reset input files
// ============================================================================

void Simulator::buildTimeline() {
    async_inputs_.clear();
    sync_input_clock_.clear();
    for (const auto& input : runtime_metadata_.input_leaves) {
        if (input.kind == RuntimeInputKind::Sync) {
            if (!input.clock_domain.has_value()) {
                throw CompilerError(std::format(
                    "Simulator: sync input '{}' has no clock domain", input.leaf_name));
            }
            sync_input_clock_[input.leaf_name] = *input.clock_domain;
        } else {
            async_inputs_.insert(input.leaf_name);
        }
    }

    // Parse async input files.
    // Scalar/struct ports: one file per leaf, format "time 0xVALUE".
    // Unpacked-array ports: one combined file per port, format "time 0xV0, 0xV1, ...".
    forEachInputNode(module_, [&](const ModuleNode& input) {
        auto leaves = moduleNodeLeafRefs(input);
        if (leaves.empty()) return;
        if (!async_inputs_.count(leaves[0].leaf_name)) return;

        const bool is_array = !input.type.unpacked_dims.empty();

        auto openFile = [&](const std::string& path) -> std::ifstream {
            std::ifstream f(path);
            if (!f.is_open())
                throw CompilerError(std::format(
                    "Simulator: cannot open async input file '{}'", path));
            return f;
        };

        if (is_array) {
            std::string path = config_.inputs_dir + "/" + input.name + ".txt";
            std::ifstream file = openFile(path);
            Type elem_type = unpackedElementType(input.type);
            if (elem_type.width <= 0)
                throw CompilerError(std::format(
                    "Simulator: async array input '{}' element has no type width", input.name));

            std::string line;
            while (std::getline(file, line)) {
                if (line.empty() || line[0] == '#') continue;
                std::istringstream iss(line);
                std::string time_token;
                if (!(iss >> time_token))
                    throw CompilerError(std::format(
                        "Simulator: bad line in async file '{}': {}", path, line));
                int64_t time = parseTimeWithUnit(time_token, path, line);

                for (size_t i = 0; i < leaves.size(); ++i) {
                    std::string hex_token;
                    if (!(iss >> hex_token))
                        throw CompilerError(std::format(
                            "Simulator: async array file '{}' line has fewer values than "
                            "expected (got {} of {}): {}", path, i, leaves.size(), line));
                    try {
                        SimValue val = SimValue::fromHexString(
                            hex_token, elem_type.width, elem_type.isSigned());
                        timeline_.push_back({time, leaves[i].leaf_name, std::move(val)});
                    } catch (const std::invalid_argument&) {
                        throw CompilerError(std::format(
                            "Simulator: async array file '{}' bad value token '{}': {}",
                            path, hex_token, line));
                    }
                    // Skip "(decimal)," debug suffix before the next element
                    char c;
                    while (iss.get(c) && c != ',') {}
                }
            }
        } else {
            for (const auto& leaf : leaves) {
                std::string path = config_.inputs_dir + "/" + leaf.leaf_name + ".txt";
                std::ifstream file = openFile(path);
                const Type& leafType = leaf.node && leaf.node->type
                    ? *leaf.node->type
                    : input.type;
                if (leafType.width <= 0)
                    throw CompilerError(std::format(
                        "Simulator: async input '{}' has no resolved type width", leaf.leaf_name));

                std::string line;
                while (std::getline(file, line)) {
                    if (line.empty() || line[0] == '#') continue;
                    std::istringstream iss(line);
                    std::string time_token, value_token;
                    if (!(iss >> time_token >> value_token))
                        throw CompilerError(std::format(
                            "Simulator: bad line in async file '{}': {}", path, line));
                    try {
                        SimValue val = SimValue::fromHexString(
                            value_token, leafType.width, leafType.isSigned());
                        int64_t time = parseTimeWithUnit(time_token, path, line);
                        timeline_.push_back({time, leaf.leaf_name, std::move(val)});
                    } catch (const std::invalid_argument&) {
                        throw CompilerError(std::format(
                            "Simulator: async file '{}' has bad value "
                            "(expected leading token like 0x1a2b, optional trailing debug text): {}",
                            path, line));
                    }
                }
            }
        }
    });

    std::stable_sort(timeline_.begin(), timeline_.end(),
        [](const AsyncEvent& a, const AsyncEvent& b) { return a.time < b.time; });
}

// ============================================================================
// Load sync input files (one value per line)
// ============================================================================

void Simulator::loadSyncInputs() {
    forEachInputNode(module_, [&](const ModuleNode& input) {
        if (!std::holds_alternative<SyncSignal>(input.sync_type)) return;
        auto leaves = moduleNodeLeafRefs(input);
        const bool is_array = !input.type.unpacked_dims.empty();

        if (is_array) {
            // One combined file per port; each line has comma-separated element values.
            std::string path = config_.inputs_dir + "/" + input.name + ".txt";
            std::ifstream file(path);
            if (!file.is_open())
                throw CompilerError(std::format(
                    "Simulator: cannot open sync input file '{}'", path));
            Type elem_type = unpackedElementType(input.type);
            if (elem_type.width <= 0)
                throw CompilerError(std::format(
                    "Simulator: sync array input '{}' element has no type width", input.name));

            // Collect per-leaf value vectors indexed by leaf index
            std::vector<std::vector<SimValue>> per_leaf(leaves.size());
            std::string line;
            while (std::getline(file, line)) {
                if (line.empty() || line[0] == '#') continue;
                std::istringstream iss(line);
                for (size_t i = 0; i < leaves.size(); ++i) {
                    std::string hex_token;
                    if (!(iss >> hex_token))
                        throw CompilerError(std::format(
                            "Simulator: sync array file '{}' line has fewer values than "
                            "expected (got {} of {}): {}", path, i, leaves.size(), line));
                    try {
                        per_leaf[i].push_back(SimValue::fromHexString(
                            hex_token, elem_type.width, elem_type.isSigned()));
                    } catch (const std::invalid_argument&) {
                        throw CompilerError(std::format(
                            "Simulator: sync array file '{}' bad value token '{}': {}",
                            path, hex_token, line));
                    }
                    // Skip "(decimal)," debug suffix before the next element
                    char c;
                    while (iss.get(c) && c != ',') {}
                }
            }
            for (size_t i = 0; i < leaves.size(); ++i) {
                if (per_leaf[i].empty())
                    throw CompilerError(std::format(
                        "Simulator: sync input file '{}' is empty", path));
                sync_input_data_[leaves[i].leaf_name] = std::move(per_leaf[i]);
                sync_input_pos_[leaves[i].leaf_name] = 0;
            }
        } else {
            for (const auto& leaf : leaves) {
                const std::string& name = leaf.leaf_name;
                std::string path = config_.inputs_dir + "/" + name + ".txt";
                std::ifstream file(path);
                if (!file.is_open())
                    throw CompilerError(std::format(
                        "Simulator: cannot open sync input file '{}'", path));

                std::vector<SimValue> values;
                std::string line;
                while (std::getline(file, line)) {
                    if (line.empty() || line[0] == '#') continue;
                    std::istringstream iss(line);
                    std::string hex_text;
                    if (!(iss >> hex_text))
                        throw CompilerError(std::format(
                            "Simulator: sync file '{}' has unparseable line: {}", path, line));
                    try {
                        const Type& leafType = leaf.node && leaf.node->type
                            ? *leaf.node->type
                            : input.type;
                        if (leafType.width <= 0)
                            throw CompilerError(std::format(
                                "Simulator: sync input '{}' has no resolved type width", name));
                        values.push_back(SimValue::fromHexString(
                            hex_text, leafType.width, leafType.isSigned()));
                    } catch (const std::invalid_argument&) {
                        throw CompilerError(std::format(
                            "Simulator: sync file '{}' has bad hex value "
                            "(expected leading token like 0x1a2b, optional trailing debug text): {}",
                            path, line));
                    }
                }
                if (values.empty())
                    throw CompilerError(std::format(
                        "Simulator: sync input file '{}' is empty", path));
                sync_input_data_[name] = std::move(values);
                sync_input_pos_[name] = 0;
            }
        }
    });
}

// ============================================================================
// Sync input advancement
// ============================================================================

SyncInputTransition Simulator::collectClockSyncInputTransition(ClockId active_clock) {
    SyncInputTransition transition;
    for (auto& [name, pos] : sync_input_pos_) {
        auto clk_it = sync_input_clock_.find(name);
        if (clk_it == sync_input_clock_.end() || clk_it->second != active_clock)
            continue;

        const auto* input = runtime_metadata_.findInput(name);
        if (!input) {
            throw CompilerError(std::format(
                "Simulator: sync input '{}' has no runtime handle", name));
        }
        const auto& data = sync_input_data_[name];
        transition.before_edge.push_back(RuntimeInputUpdate{
            .input = input->id,
            .value = data[pos],
        });

        if (pos + 1 < data.size()) {
            pos++;
        }
        transition.after_edge.push_back(RuntimeInputUpdate{
            .input = input->id,
            .value = data[pos],
        });
    }
    return transition;
}

bool Simulator::isClockOrResetSource(const std::string& leaf_name) const {
    return runtime_metadata_.clock_domains_by_top_input.contains(leaf_name) ||
           runtime_metadata_.reset_domains_by_top_input.contains(leaf_name);
}

std::optional<edge_t> Simulator::updateAsyncInputAndDetectEdge(
    const RuntimeInputUpdate& update,
    int64_t time_ns) {
    if (update.input.value >= runtime_metadata_.input_leaves.size()) {
        throw CompilerError(std::format(
            "Simulator: invalid async input handle {}", update.input.value));
    }

    const auto& input = runtime_metadata_.input_leaves.at(update.input.value);
    auto value_it = async_input_values_.find(input.leaf_name);
    if (value_it == async_input_values_.end()) {
        throw CompilerError(std::format(
            "Simulator: async input '{}' has no initialized value before time {}",
            input.leaf_name, time_ns));
    }

    const SimValue old_value = value_it->second;
    value_it->second = update.value;

    if (!isClockOrResetSource(input.leaf_name)) {
        return std::nullopt;
    }
    if (old_value.eq(update.value)) {
        return std::nullopt;
    }
    if (old_value.width() != 1 || update.value.width() != 1) {
        throw CompilerError(std::format(
            "Simulator: clock/reset input '{}' must be scalar binary at time {}",
            input.leaf_name, time_ns));
    }

    const bool old_zero = old_value.isZero();
    const bool old_one = !old_zero && old_value.lowU64() == 1;
    const bool new_zero = update.value.isZero();
    const bool new_one = !new_zero && update.value.lowU64() == 1;
    if (!old_one && !old_zero) {
        throw CompilerError(std::format(
            "Simulator: clock/reset input '{}' previous value is not binary at time {}",
            input.leaf_name, time_ns));
    }
    if (!new_one && !new_zero) {
        throw CompilerError(std::format(
            "Simulator: clock/reset input '{}' new value is not binary at time {}",
            input.leaf_name, time_ns));
    }

    if (old_zero && new_one) return POSEDGE;
    if (old_one && new_zero) return NEGEDGE;

    throw CompilerError(std::format(
        "Simulator: clock/reset input '{}' changed without a binary edge at time {}",
        input.leaf_name, time_ns));
}

// ============================================================================
// Output recording
// ============================================================================

void Simulator::recordOutputs() {
    for (const auto& output : runtime_metadata_.output_leaves) {
        recorded_values_[output.leaf_name].push_back(runtime_->getOutput(output.id));
    }
}

void Simulator::writeOutputFiles() {
    std::filesystem::create_directories(config_.output_dir);

    for (const auto& [name, values] : recorded_values_) {
        std::string filepath = config_.output_dir + "/" + name + ".txt";
        std::ofstream out(filepath);
        if (!out.is_open()) {
            throw CompilerError(std::format(
                "Simulator: cannot open output file '{}'", filepath));
        }
        for (const SimValue& v : values) {
            out << v.toBinaryString() << "\n";
        }
    }
}


// ============================================================================
// Constructor
// ============================================================================

Simulator::Simulator(const RtlRuntimeModel& model, const SimConfig& config)
    : model_(model),
      ir_(model.ir()),
      module_(model.top()),
      config_(config),
      runtime_metadata_(model.metadata())
{
    if (!module_.dfg) {
        throw CompilerError("Simulator: module has no DFG");
    }

    runtime_ = std::make_unique<NativeSimEngine>(model_, config_);
    buildTimeline();
    loadSyncInputs();
}

// ============================================================================
// Main simulation loop
// ============================================================================

void Simulator::run() {
    std::cout << "Simulator: starting simulation for module '" << module_.name << "'" << std::endl;

    // === VCD Setup ===
    vcd_ = std::make_unique<VcdWriter>(ir_, runtime_metadata_, config_.output_dir);

    // === Initialization (time 0) ===

    // 1. Initialize flop .q values (recursive)
    uint64_t rng_seed = 0;
    {
        if (config_.flops_initial == FlopsInitial::Random) {
            rng_seed = config_.flops_initial_seed.value_or(std::random_device{}());
        }
        std::mt19937_64 rng(rng_seed);
        runtime_->initialize(config_.flops_initial, config_.flops_use_initial_values, rng);
    }

    // 2. Gather initial async input values from the first event in their
    // timeline (must be at time 0).
    std::vector<RuntimeInputUpdate> initial_async_inputs;
    async_input_values_.clear();
    for (const auto& name : async_inputs_) {
        bool found = false;
        for (const auto& evt : timeline_) {
            if (evt.signal_name == name) {
                if (evt.time != 0) {
                    throw CompilerError(std::format(
                        "Simulator: async input '{}' first event is at time {} (must be 0)",
                        name, evt.time));
                }
                const auto* input = runtime_metadata_.findInput(name);
                if (!input) {
                    throw CompilerError(std::format(
                        "Simulator: async input '{}' has no runtime handle", name));
                }
                initial_async_inputs.push_back(RuntimeInputUpdate{
                    .input = input->id,
                    .value = evt.value,
                });
                async_input_values_[name] = evt.value;
                found = true;
                break;
            }
        }
        if (!found) {
            throw CompilerError(std::format(
                "Simulator: async input '{}' has no events in timeline", name));
        }
    }

    // 3. Gather initial sync input values from the first line of their files.
    std::vector<RuntimeInputUpdate> initial_sync_inputs;
    for (const auto& [name, data] : sync_input_data_) {
        const auto* input = runtime_metadata_.findInput(name);
        if (!input) {
            throw CompilerError(std::format(
                "Simulator: sync input '{}' has no runtime handle", name));
        }
        initial_sync_inputs.push_back(RuntimeInputUpdate{
            .input = input->id,
            .value = data[0],
        });
    }

    runtime_->initializeInputsAndEvaluate(initial_async_inputs, initial_sync_inputs);

    // VCD: trace initial state at time 0
    vcd_->update(*runtime_, 0);

    std::cout << "Simulator: initialization complete, processing "
              << timeline_.size() << " async events" << std::endl;

    recordOutputs();

    // === Main loop: process timeline one timestamp group at a time ===

    const size_t total_events = timeline_.size();
    const int progress_interval_pct = 10;
    using Clock = std::chrono::steady_clock;
    using Sec = std::chrono::duration<double>;
    auto sim_start     = Clock::now();
    auto step_start    = Clock::now();
    size_t step_events = 0;
    int    last_pct    = 0;
    std::mt19937_64 same_time_order_rng(0);

    size_t idx = 0;
    while (idx < timeline_.size()) {
        int64_t batch_time = timeline_[idx].time;

        // Collect all events at this time
        std::vector<const AsyncEvent*> batch;
        while (idx < timeline_.size() && timeline_[idx].time == batch_time) {
            batch.push_back(&timeline_[idx]);
            idx++;
        }

        step_events += batch.size();

        if (total_events > 0) {
            int cur_pct = static_cast<int>((idx * 100) / total_events);
            cur_pct = (cur_pct / progress_interval_pct) * progress_interval_pct;
            if (cur_pct >= last_pct + progress_interval_pct) {
                auto now          = Clock::now();
                double step_secs  = std::chrono::duration_cast<Sec>(now - step_start).count();
                double total_secs = std::chrono::duration_cast<Sec>(now - sim_start).count();
                double step_eps   = step_secs  > 0 ? step_events / step_secs  : 0;
                double avg_eps    = total_secs > 0 ? idx          / total_secs : 0;
                std::cout << std::format(
                    "Simulator: {:3d}%  step {:.0f} ev/s  avg {:.0f} ev/s\n",
                    cur_pct, step_eps, avg_eps);
                last_pct    = cur_pct;
                step_start  = now;
                step_events = 0;
            }
        }

        // Build a map from signal name to last event, so duplicate timestamps keep the last value.
        std::map<std::string, const AsyncEvent*> last_event_for_signal;
        for (const auto* evt : batch) {
            last_event_for_signal[evt->signal_name] = evt;
        }

        std::vector<RuntimeInputUpdate> async_updates;
        for (const auto& [name, evt] : last_event_for_signal) {
            const auto* input = runtime_metadata_.findInput(evt->signal_name);
            if (!input) {
                throw CompilerError(std::format(
                    "Simulator: async input '{}' has no runtime handle", evt->signal_name));
            }
            async_updates.push_back(RuntimeInputUpdate{
                .input = input->id,
                .value = evt->value,
            });
        }

        std::shuffle(async_updates.begin(), async_updates.end(), same_time_order_rng);

        bool timestamp_had_active_clock_edge = false;
        for (const auto& update : async_updates) {
            const auto& input_metadata = runtime_metadata_.input_leaves.at(update.input.value);
            const std::string& leaf_name = input_metadata.leaf_name;
            const auto clock_it = runtime_metadata_.clock_domains_by_top_input.find(leaf_name);
            const auto reset_it = runtime_metadata_.reset_domains_by_top_input.find(leaf_name);
            const bool is_clock_source =
                clock_it != runtime_metadata_.clock_domains_by_top_input.end();
            const bool is_reset_source =
                reset_it != runtime_metadata_.reset_domains_by_top_input.end();

            std::optional<edge_t> edge = updateAsyncInputAndDetectEdge(update, batch_time);
            if (is_clock_source && is_reset_source) {
                throw CompilerError(std::format(
                    "Simulator: async input '{}' is both a clock and reset source; "
                    "edge ordering is ambiguous",
                    leaf_name));
            }

            if (is_clock_source) {
                if (!edge.has_value()) continue;
                for (ClockId clock_id : clock_it->second) {
                    SyncInputTransition sync_transition;
                    if (*edge == ir_.clocks.at(clock_id.value).edge) {
                        sync_transition = collectClockSyncInputTransition(clock_id);
                        timestamp_had_active_clock_edge = true;
                    }
                    runtime_->applyClockEdge(clock_id, *edge,
                                             sync_transition.before_edge);
                    if (!sync_transition.after_edge.empty()) {
                        runtime_->setInputValues(sync_transition.after_edge);
                    }
                }
                continue;
            }

            if (is_reset_source) {
                if (!edge.has_value()) continue;
                for (ResetId reset_id : reset_it->second) {
                    runtime_->applyResetEdge(reset_id, *edge);
                }
                continue;
            }

            if (input_metadata.kind != RuntimeInputKind::Async) {
                throw CompilerError(std::format(
                    "Simulator: input '{}' cannot be driven as a plain async signal",
                    leaf_name));
            }
            runtime_->setInputValues(std::span<const RuntimeInputUpdate>(&update, 1));
        }
        // VCD: trace all values at every time step
        vcd_->update(*runtime_, batch_time);

        // Record output values only on active clock edges (for text output)
        if (timestamp_had_active_clock_edge) {
            recordOutputs();
        }
    }

    {
        double total_secs = std::chrono::duration_cast<Sec>(Clock::now() - sim_start).count();
        double avg_eps    = total_secs > 0 ? total_events / total_secs : 0;
        std::cout << std::format(
            "Simulator: 100%  total {:.0f} ev/s avg over {} events\n",
            avg_eps, total_events);
    }

    vcd_->close(timeline_.empty() ? 0 : timeline_.back().time);

    writeOutputFiles();

    std::cout << "Simulator: simulation complete. "
              << recorded_values_.begin()->second.size() << " cycles recorded."
              << std::endl;
    if (config_.flops_initial == FlopsInitial::Random) {
        std::cout << "Simulator: flops-initial seed = " << rng_seed << std::endl;
    }
    std::cout << "Simulator: output written to '" << config_.output_dir << "/'" << std::endl;
    std::cout << "Simulator: grouped VCD trace written to '" << vcd_->grouped_path() << "'" << std::endl;
}

} // namespace mate
