#include "sim/simulator.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <queue>
#include <random>
#include <set>
#include <sstream>


namespace custom_hdl {

// ============================================================================
// Bit mask helper
// ============================================================================

int64_t Simulator::maskToWidth(int64_t val, const DFGNode* node) {
    if (!node->type.has_value() || node->type->width <= 0)
        return val;

    int w = node->type->width;
    // Width > 64 is rejected in constructor, so w is in [1, 64]
    uint64_t mask = (w == 64) ? ~0ULL : (1ULL << w) - 1;
    val &= static_cast<int64_t>(mask);

    // Sign-extend if signed
    if (node->type->isSigned() && w < 64 && (val & (1LL << (w - 1)))) {
        val |= ~static_cast<int64_t>(mask);
    }
    return val;
}

// ============================================================================
// Topological sort (Kahn's algorithm)
// ============================================================================

void Simulator::buildTopology() {
    const auto& nodes = module_.dfg->nodes;

    // Build flop .q node -> FlopInfo map (needed by run loop, not by topo sort)
    for (const auto& flop : module_.flops) {
        std::string qname = flop.name + ".q";
        if (auto it = module_.dfg->signals.find(qname); it != module_.dfg->signals.end()) {
            flop_q_nodes_[it->second] = &flop;
        }
    }

    // The DFG is purely combinational (no cycles). Kahn's algorithm gives eval order.
    std::map<const DFGNode*, int> in_degree;
    std::map<const DFGNode*, std::vector<const DFGNode*>> successors;

    for (const auto& node : nodes) {
        in_degree[node.get()] = 0;
    }

    for (const auto& node : nodes) {
        for (const auto& input : node->in) {
            in_degree[node.get()]++;
            successors[input.node].push_back(node.get());
        }
    }

    std::queue<const DFGNode*> q;
    for (const auto& [node, deg] : in_degree) {
        if (deg == 0) q.push(node);
    }

    topo_order_.clear();
    while (!q.empty()) {
        const DFGNode* curr = q.front();
        q.pop();
        topo_order_.push_back(curr);

        for (const DFGNode* succ : successors[curr]) {
            if (--in_degree[succ] == 0) {
                q.push(succ);
            }
        }
    }

    if (topo_order_.size() != nodes.size()) {
        throw CompilerError(std::format(
            "Simulator: topological sort failed — {} of {} nodes sorted (cycle in DFG?)",
            topo_order_.size(), nodes.size()));
    }
}

// Parse a time token like "5ns" or "1.5us" into integer nanoseconds.
static int64_t parseTimeWithUnit(const std::string& token,
                                 const std::string& file_path,
                                 const std::string& line) {
    // Find where the unit suffix starts (scan from end for alphabetic chars)
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
    // Determine which inputs are async (clocks and resets)
    for (const auto& flop : module_.flops) {
        async_inputs_.insert(flop.clock.name);
        if (flop.reset.has_value()) {
            async_inputs_.insert(flop.reset->name);
        }
    }

    // Parse async input files: "time value" format per line
    for (const auto& name : async_inputs_) {
        std::string path = config_.inputs_dir + "/" + name + ".txt";

        std::ifstream file(path);
        if (!file.is_open()) {
            throw CompilerError(std::format(
                "Simulator: cannot open async input file '{}'", path));
        }

        std::string line;
        while (std::getline(file, line)) {
            if (line.empty() || line[0] == '#') continue;
            std::istringstream iss(line);
            std::string time_token;
            int64_t value;
            if (!(iss >> time_token >> value)) {
                throw CompilerError(std::format(
                    "Simulator: bad line in async file '{}': {}", path, line));
            }
            int64_t time = parseTimeWithUnit(time_token, path, line);
            timeline_.push_back({time, name, value});
        }
    }

    // Sort by time (stable sort preserves file order for same-time events)
    std::stable_sort(timeline_.begin(), timeline_.end(),
        [](const AsyncEvent& a, const AsyncEvent& b) { return a.time < b.time; });
}

// ============================================================================
// Build flop lookup maps
// ============================================================================

void Simulator::buildFlopMaps() {
    for (const auto& flop : module_.flops) {
        flops_by_clock_[flop.clock.name].push_back(&flop);
        if (flop.reset.has_value()) {
            flops_by_reset_[flop.reset->name].push_back(&flop);
        }
    }
}

// ============================================================================
// Load sync input files (one value per line)
// ============================================================================

void Simulator::loadSyncInputs() {
    for (const auto& [name, node] : module_.dfg->inputs) {
        if (async_inputs_.count(name)) continue;  // skip async inputs

        std::string path = config_.inputs_dir + "/" + name + ".txt";
        std::ifstream file(path);
        if (!file.is_open()) {
            throw CompilerError(std::format(
                "Simulator: cannot open sync input file '{}'", path));
        }

        std::vector<int64_t> values;
        std::string line;
        while (std::getline(file, line)) {
            if (line.empty() || line[0] == '#') continue;
            values.push_back(std::stoll(line));
        }

        if (values.empty()) {
            throw CompilerError(std::format(
                "Simulator: sync input file '{}' is empty", path));
        }

        sync_input_data_[name] = std::move(values);
        sync_input_pos_[name] = 0;
    }
}

// ============================================================================
// Node evaluation
// ============================================================================

int64_t Simulator::evaluateNode(const DFGNode* node) {
    auto getVal = [&](int idx) -> int64_t {
        return values_.at(node->in[idx].node);
    };

    switch (node->op) {
        case DFGOp::INPUT:
        case DFGOp::CONST:
            // Pre-set, should already be in values_
            return values_.at(node);

        case DFGOp::SIGNAL:
        case DFGOp::OUTPUT:
            if (node->in.empty()) return values_.at(node);
            return getVal(0);

        case DFGOp::ADD:   return getVal(0) + getVal(1);
        case DFGOp::SUB:   return getVal(0) - getVal(1);
        case DFGOp::MUL:   return getVal(0) * getVal(1);
        case DFGOp::DIV: {
            int64_t divisor = getVal(1);
            if (divisor == 0)
                throw CompilerError("Simulator: division by zero", node);
            return getVal(0) / divisor;
        }
        case DFGOp::POWER: {
            int64_t base = getVal(0);
            int64_t exp = getVal(1);
            if (exp < 0) return 0;
            int64_t result = 1;
            for (int64_t i = 0; i < exp; i++) result *= base;
            return result;
        }

        case DFGOp::EQ:  return getVal(0) == getVal(1) ? 1 : 0;
        case DFGOp::LT:  return getVal(0) <  getVal(1) ? 1 : 0;
        case DFGOp::LE:  return getVal(0) <= getVal(1) ? 1 : 0;
        case DFGOp::GT:  return getVal(0) >  getVal(1) ? 1 : 0;
        case DFGOp::GE:  return getVal(0) >= getVal(1) ? 1 : 0;

        case DFGOp::SHL:  return getVal(0) << getVal(1);
        case DFGOp::ASR:  return getVal(0) >> getVal(1);  // arithmetic if signed (C++ guarantees for signed)

        case DFGOp::MUX:
            return getVal(0) ? getVal(1) : getVal(2);

        case DFGOp::MUX_N: {
            int n = static_cast<int>(node->in.size()) / 2;
            // First active one-hot selector picks its data value
            for (int i = 0; i < n; i++) {
                if (values_.at(node->in[i].node) != 0) {
                    return values_.at(node->in[n + i].node);
                }
            }
            // No selector active — return last data value as default
            return values_.at(node->in[2 * n - 1].node);
        }

        case DFGOp::UNARY_PLUS:    return getVal(0);
        case DFGOp::UNARY_NEGATE:  return -getVal(0);
        case DFGOp::BITWISE_NOT:   return ~getVal(0);
        case DFGOp::LOGICAL_NOT:   return getVal(0) == 0 ? 1 : 0;

        case DFGOp::REDUCTION_AND: {
            int w = node->in[0].node->type.has_value() ? node->in[0].node->type->width : 64;
            uint64_t mask = (w == 64) ? ~0ULL : (1ULL << w) - 1;
            return (static_cast<uint64_t>(getVal(0)) & mask) == mask ? 1 : 0;
        }
        case DFGOp::REDUCTION_NAND: {
            int w = node->in[0].node->type.has_value() ? node->in[0].node->type->width : 64;
            uint64_t mask = (w == 64) ? ~0ULL : (1ULL << w) - 1;
            return (static_cast<uint64_t>(getVal(0)) & mask) == mask ? 0 : 1;
        }
        case DFGOp::REDUCTION_OR:
            return getVal(0) != 0 ? 1 : 0;
        case DFGOp::REDUCTION_NOR:
            return getVal(0) == 0 ? 1 : 0;
        case DFGOp::REDUCTION_XOR: {
            uint64_t v = static_cast<uint64_t>(getVal(0));
            int w = node->in[0].node->type.has_value() ? node->in[0].node->type->width : 64;
            if (w < 64) v &= (1ULL << w) - 1;
            return std::popcount(v) & 1;
        }
        case DFGOp::REDUCTION_XNOR: {
            uint64_t v = static_cast<uint64_t>(getVal(0));
            int w = node->in[0].node->type.has_value() ? node->in[0].node->type->width : 64;
            if (w < 64) v &= (1ULL << w) - 1;
            return (std::popcount(v) & 1) ^ 1;
        }

        case DFGOp::INDEX: {
            const DFGNode* source_node = node->in[0].node;

            // Vector indexing (unpacked dimension): select the value of the
            // i-th input node. Use modulo to wrap out-of-range indices.
            // high and low are equal for vector selects, so either works.
            if (source_node->type.has_value() && !source_node->type->unpacked_dims.empty()) {
                int64_t index = getVal(1);
                int64_t n = static_cast<int64_t>(source_node->in.size());
                index = ((index % n) + n) % n;
                return values_.at(source_node->in[index].node);
            }

            // Bit/range select (packed dimension)
            int64_t high = getVal(1);
            int64_t low = getVal(2);
            int64_t source_val = getVal(0);
            int64_t width = high - low + 1;

            // Compute physical bit position from packed dimension bounds
            int64_t bit_pos;
            if (source_node->type.has_value() && !source_node->type->packed_dims.empty()) {
                const auto& dim = source_node->type->packed_dims[0];
                if (dim.left >= dim.right) {
                    bit_pos = low - dim.right;
                } else {
                    bit_pos = dim.right - high;
                }
            } else {
                bit_pos = low;
            }

            uint64_t elem_mask = (width >= 64) ? ~0ULL : (1ULL << width) - 1;
            return static_cast<int64_t>((static_cast<uint64_t>(source_val) >> bit_pos) & elem_mask);
        }

        case DFGOp::CONCAT: {
            // Shift-and-or each input value into the result, MSB-first
            uint64_t result = 0;
            for (size_t i = 0; i < node->in.size(); ++i) {
                const DFGNode* inputNode = node->in[i].node;
                int w = inputNode->type.has_value() ? inputNode->type->width : 64;
                uint64_t mask = (w >= 64) ? ~0ULL : (1ULL << w) - 1;
                result = (result << w) | (static_cast<uint64_t>(values_.at(inputNode)) & mask);
            }
            return static_cast<int64_t>(result);
        }

        case DFGOp::CONCAT_ALIGN:
            throw CompilerError("Simulator: CONCAT_ALIGN should have been cleaned up by concat_cleanup pass", node);

        case DFGOp::MODULE:
            throw CompilerError("Simulator: MODULE nodes not supported (no hierarchy support yet)", node);
    }

    throw CompilerError(std::format("Simulator: unhandled op {}", to_string(node->op)), node);
}

void Simulator::evaluateCombinational() {
    for (const DFGNode* node : topo_order_) {
        // Skip pre-set nodes
        if (node->op == DFGOp::INPUT || node->op == DFGOp::CONST) continue;
        // Skip flop .q nodes (they hold register values)
        if (flop_q_nodes_.count(node)) continue;

        int64_t val = evaluateNode(node);
        values_[node] = maskToWidth(val, node);
    }
}

// ============================================================================
// Sync input advancement
// ============================================================================

void Simulator::advanceSyncInputs() {
    for (auto& [name, pos] : sync_input_pos_) {
        if (pos + 1 < sync_input_data_[name].size()) {
            pos++;
        }
        // Set the value in the DFG input node
        auto it = module_.dfg->inputs.find(name);
        if (it != module_.dfg->inputs.end()) {
            values_[it->second] = sync_input_data_[name][pos];
        }
    }
}

// ============================================================================
// Output recording
// ============================================================================

void Simulator::recordOutputs() {
    // Record all non-async inputs
    /*
    for (const auto& [name, node] : module_.dfg->inputs) {
        if (!async_inputs_.count(name)) {
            recorded_values_[name].push_back(values_.at(node));
        }
    }
    */

    // Record all signals (except flop .q which are internal)
    /*
    for (const auto& [name, node] : module_.dfg->signals) {
        recorded_values_[name].push_back(values_.at(node));
    }
    */

    // Record all outputs
    for (const auto& [name, node] : module_.dfg->outputs) {
        recorded_values_[name].push_back(values_.at(node));
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
        for (int64_t v : values) {
            out << v << "\n";
        }
    }
}

// ============================================================================
// VCD tracing
// ============================================================================

void Simulator::setupVcd(std::ofstream& vcd_out) {
    vcd_top_ = std::make_unique<vcd_tracer::top>(module_.name);

    // Helper: get the bit width from a DFG node's type
    auto getWidth = [](const DFGNode* node) -> unsigned int {
        if (node->type.has_value() && node->type->width > 0)
            return static_cast<unsigned int>(node->type->width);
        return 64;
    };

    // Helper: elaborate a value<int64_t> into a module with the correct runtime
    // bit width.  We wrap the module's add_fn to override the bit_size that
    // value<int64_t>::elaborate() would hardcode to 64.
    auto elaborateWithWidth = [](vcd_tracer::module& mod, vcd_tracer::value_base& var,
                                 std::string_view name, unsigned int width) {
        auto original_add_fn = mod.get_add_fn();
        auto override_fn = [original_add_fn, width](
                std::string_view var_name, std::string_view var_type,
                unsigned int /*bit_size*/, vcd_tracer::scope_fn::dumper_fn fn)
                -> vcd_tracer::value_context {
            return original_add_fn(var_name, var_type, width, fn);
        };
        var.elaborate(override_fn, name);
    };

    // Build set of flop .d/.q signal names to exclude from the signals scope,
    // and collect .q signals to trace under the flops scope.
    // Detect flop pairs by finding signals ending in .d that have a .q counterpart.
    std::set<std::string> flop_signal_names;
    std::map<std::string, const DFGNode*> flop_q_entries;  // base name -> .q node
    for (const auto& [name, node] : module_.dfg->signals) {
        if (name.size() > 2 && name.substr(name.size() - 2) == ".d") {
            std::string base = name.substr(0, name.size() - 2);
            auto qit = module_.dfg->signals.find(base + ".q");
            if (qit != module_.dfg->signals.end()) {
                flop_signal_names.insert(name);
                flop_signal_names.insert(base + ".q");
                flop_q_entries[base] = qit->second;
            }
        }
    }

    // Remove vector parent entries that have indexed children (e.g. remove
    // "my_vector_flop" when "my_vector_flop[0]" exists), since the parent
    // is just the aggregate — only the individual elements should be traced.
    std::vector<std::string> parents_to_remove;
    for (const auto& [base, _] : flop_q_entries) {
        if (flop_q_entries.count(base + "[0]")) {
            parents_to_remove.push_back(base);
        }
    }
    for (const auto& p : parents_to_remove) {
        flop_q_entries.erase(p);
    }

    // Helper: elaborate params into a scope and set their constant values
    auto elaborateParams = [&](vcd_tracer::module& mod,
                               std::vector<std::unique_ptr<vcd_tracer::value<int64_t>>>& dest) {
        for (const auto& param : module_.parameters) {
            unsigned int w = param.type.width > 0 ? static_cast<unsigned int>(param.type.width) : 32;
            auto v = std::make_unique<vcd_tracer::value<int64_t>>();
            elaborateWithWidth(mod, *v, param.name, w);
            v->set_runtime_bit_size(w);
            v->set(static_cast<int64_t>(param.value));
            dest.push_back(std::move(v));
        }
        for (const auto& param : module_.localparams) {
            unsigned int w = param.type.width > 0 ? static_cast<unsigned int>(param.type.width) : 32;
            auto v = std::make_unique<vcd_tracer::value<int64_t>>();
            elaborateWithWidth(mod, *v, param.name, w);
            v->set_runtime_bit_size(w);
            v->set(static_cast<int64_t>(param.value));
            dest.push_back(std::move(v));
        }
    };

    {
        vcd_tracer::module inputs_mod(vcd_top_->root, "inputs");
        vcd_tracer::module signals_mod(vcd_top_->root, "signals");
        vcd_tracer::module flops_mod(vcd_top_->root, "flops");
        vcd_tracer::module outputs_mod(vcd_top_->root, "outputs");
        vcd_tracer::module params_mod(vcd_top_->root, "params");

        for (const auto& [name, node] : module_.dfg->inputs) {
            unsigned int w = getWidth(node);
            auto v = std::make_unique<vcd_tracer::value<int64_t>>();
            elaborateWithWidth(inputs_mod, *v, name, w);
            v->set_runtime_bit_size(w);
            vcd_values_[node] = std::move(v);
        }

        // Async inputs (clocks/resets) that don't have DFG input nodes
        for (const auto& name : async_inputs_) {
            if (module_.dfg->inputs.count(name)) continue;  // already added above
            auto v = std::make_unique<vcd_tracer::value<int64_t>>();
            elaborateWithWidth(inputs_mod, *v, name, 1);
            v->set_runtime_bit_size(1);
            vcd_async_values_[name] = std::move(v);
        }

        for (const auto& [name, node] : module_.dfg->signals) {
            if (flop_signal_names.count(name)) continue;  // skip flop .d/.q
            unsigned int w = getWidth(node);
            auto v = std::make_unique<vcd_tracer::value<int64_t>>();
            elaborateWithWidth(signals_mod, *v, name, w);
            v->set_runtime_bit_size(w);
            vcd_values_[node] = std::move(v);
        }

        // Flops: trace .q value under the bare flop name
        for (const auto& [base_name, qnode] : flop_q_entries) {
            unsigned int w = getWidth(qnode);
            auto v = std::make_unique<vcd_tracer::value<int64_t>>();
            elaborateWithWidth(flops_mod, *v, base_name, w);
            v->set_runtime_bit_size(w);
            vcd_values_[qnode] = std::move(v);
        }

        for (const auto& [name, node] : module_.dfg->outputs) {
            unsigned int w = getWidth(node);
            auto v = std::make_unique<vcd_tracer::value<int64_t>>();
            elaborateWithWidth(outputs_mod, *v, name, w);
            v->set_runtime_bit_size(w);
            vcd_values_[node] = std::move(v);
        }

        elaborateParams(params_mod, vcd_params_);
    }

    vcd_top_->finalize_header(vcd_out,
                              std::chrono::system_clock::from_time_t(0));
}

void Simulator::setupVcdFlat(std::ofstream& vcd_out) {
    vcd_flat_top_ = std::make_unique<vcd_tracer::top>(module_.name);

    auto getWidth = [](const DFGNode* node) -> unsigned int {
        if (node->type.has_value() && node->type->width > 0)
            return static_cast<unsigned int>(node->type->width);
        return 64;
    };

    auto elaborateWithWidth = [](vcd_tracer::module& mod, vcd_tracer::value_base& var,
                                 std::string_view name, unsigned int width) {
        auto original_add_fn = mod.get_add_fn();
        auto override_fn = [original_add_fn, width](
                std::string_view var_name, std::string_view var_type,
                unsigned int /*bit_size*/, vcd_tracer::scope_fn::dumper_fn fn)
                -> vcd_tracer::value_context {
            return original_add_fn(var_name, var_type, width, fn);
        };
        var.elaborate(override_fn, name);
    };

    // Detect flop .d/.q pairs to exclude
    std::set<std::string> flop_signal_names;
    std::map<std::string, const DFGNode*> flop_q_entries;  // base name -> .q node
    for (const auto& [name, node] : module_.dfg->signals) {
        if (name.size() > 2 && name.substr(name.size() - 2) == ".d") {
            std::string base = name.substr(0, name.size() - 2);
            auto qit = module_.dfg->signals.find(base + ".q");
            if (qit != module_.dfg->signals.end()) {
                flop_signal_names.insert(name);
                flop_signal_names.insert(base + ".q");
                flop_q_entries[base] = qit->second;
            }
        }
    }

    // Remove vector parent entries that have indexed children
    std::vector<std::string> parents_to_remove;
    for (const auto& [base, _] : flop_q_entries) {
        if (flop_q_entries.count(base + "[0]")) {
            parents_to_remove.push_back(base);
        }
    }
    for (const auto& p : parents_to_remove) {
        flop_q_entries.erase(p);
    }

    // All signals go directly into the root module (no sub-scopes)
    for (const auto& [name, node] : module_.dfg->inputs) {
        unsigned int w = getWidth(node);
        auto v = std::make_unique<vcd_tracer::value<int64_t>>();
        elaborateWithWidth(vcd_flat_top_->root, *v, name, w);
        v->set_runtime_bit_size(w);
        vcd_flat_values_[node] = std::move(v);
    }

    for (const auto& name : async_inputs_) {
        if (module_.dfg->inputs.count(name)) continue;
        auto v = std::make_unique<vcd_tracer::value<int64_t>>();
        elaborateWithWidth(vcd_flat_top_->root, *v, name, 1);
        v->set_runtime_bit_size(1);
        vcd_flat_async_values_[name] = std::move(v);
    }

    for (const auto& [name, node] : module_.dfg->signals) {
        if (flop_signal_names.count(name)) continue;  // skip flop .d/.q
        unsigned int w = getWidth(node);
        auto v = std::make_unique<vcd_tracer::value<int64_t>>();
        elaborateWithWidth(vcd_flat_top_->root, *v, name, w);
        v->set_runtime_bit_size(w);
        vcd_flat_values_[node] = std::move(v);
    }

    // Flops: trace .q value under the bare flop name
    for (const auto& [base_name, qnode] : flop_q_entries) {
        unsigned int w = getWidth(qnode);
        auto v = std::make_unique<vcd_tracer::value<int64_t>>();
        elaborateWithWidth(vcd_flat_top_->root, *v, base_name, w);
        v->set_runtime_bit_size(w);
        vcd_flat_values_[qnode] = std::move(v);
    }

    for (const auto& [name, node] : module_.dfg->outputs) {
        unsigned int w = getWidth(node);
        auto v = std::make_unique<vcd_tracer::value<int64_t>>();
        elaborateWithWidth(vcd_flat_top_->root, *v, name, w);
        v->set_runtime_bit_size(w);
        vcd_flat_values_[node] = std::move(v);
    }

    // Params: set constant values at elaboration time
    for (const auto* params : {&module_.parameters, &module_.localparams}) {
        for (const auto& param : *params) {
            unsigned int w = param.type.width > 0 ? static_cast<unsigned int>(param.type.width) : 32;
            auto v = std::make_unique<vcd_tracer::value<int64_t>>();
            elaborateWithWidth(vcd_flat_top_->root, *v, param.name, w);
            v->set_runtime_bit_size(w);
            v->set(static_cast<int64_t>(param.value));
            vcd_flat_params_.push_back(std::move(v));
        }
    }

    vcd_flat_top_->finalize_header(vcd_out,
                                   std::chrono::system_clock::from_time_t(0));
}

void Simulator::updateVcdValuesFlat(std::ofstream& vcd_out, int64_t time_ns,
                                    const std::map<std::string, int64_t>& async_values) {
    vcd_flat_top_->time_update_abs(vcd_out, std::chrono::nanoseconds{time_ns});

    for (auto& [node, vcd_val] : vcd_flat_values_) {
        vcd_val->set(values_.at(node));
    }
    for (auto& [name, vcd_val] : vcd_flat_async_values_) {
        auto it = async_values.find(name);
        if (it != async_values.end()) {
            vcd_val->set(it->second);
        }
    }
}

void Simulator::updateVcdValues(std::ofstream& vcd_out, int64_t time_ns,
                                const std::map<std::string, int64_t>& async_values) {
    // Advance time first: this dumps any pending values from the previous step
    // at the previous timestamp, then writes the new #time marker.
    vcd_top_->time_update_abs(vcd_out, std::chrono::nanoseconds{time_ns});

    // Set new values — they stay pending until the next time_update call,
    // so they'll be dumped under THIS timestamp.  The runtime_bit_size set
    // during elaboration controls how many bits the VCD dump outputs.
    for (auto& [node, vcd_val] : vcd_values_) {
        vcd_val->set(values_.at(node));
    }
    for (auto& [name, vcd_val] : vcd_async_values_) {
        auto it = async_values.find(name);
        if (it != async_values.end()) {
            vcd_val->set(it->second);
        }
    }
}

// ============================================================================
// Constructor
// ============================================================================

Simulator::Simulator(const ResolvedModule& module, const SimConfig& config)
    : module_(module), config_(config)
{
    if (!module_.dfg) {
        throw CompilerError("Simulator: module has no DFG");
    }

    // Check for MODULE nodes and width limits
    for (const auto& node : module_.dfg->nodes) {
        if (node->op == DFGOp::MODULE) {
            throw CompilerError("Simulator: MODULE nodes not supported (no hierarchy support yet)", node.get());
        }
        if (node->type.has_value() && node->type->width > 64) {
            throw CompilerError(std::format(
                "Simulator: node '{}' has width {} (max 64 supported)",
                node->str(), node->type->width), node.get());
        }
    }

    buildFlopMaps();
    buildTimeline();
    loadSyncInputs();
    buildTopology();
}

// ============================================================================
// Main simulation loop
// ============================================================================

void Simulator::run() {
    std::cout << "Simulator: starting simulation for module '" << module_.name << "'" << std::endl;

    // === VCD Setup ===
    std::filesystem::create_directories(config_.output_dir);

    std::string vcd_path = config_.output_dir + "/" + module_.name + ".vcd";
    std::ofstream vcd_out(vcd_path);
    if (!vcd_out.is_open()) {
        throw CompilerError(std::format(
            "Simulator: cannot open VCD output file '{}'", vcd_path));
    }
    setupVcd(vcd_out);

    std::string vcd_flat_path = config_.output_dir + "/" + module_.name + "-flatten.vcd";
    std::ofstream vcd_flat_out(vcd_flat_path);
    if (!vcd_flat_out.is_open()) {
        throw CompilerError(std::format(
            "Simulator: cannot open flat VCD output file '{}'", vcd_flat_path));
    }
    setupVcdFlat(vcd_flat_out);

    // === Initialization (time 0) ===

    // 1. Set all CONST node values
    for (const auto& node : module_.dfg->nodes) {
        if (node->op == DFGOp::CONST) {
            values_[node.get()] = std::get<int64_t>(node->data);
        }
    }

    // 2. Set flop .q initial values
    uint64_t rng_seed = 0;
    {
        if (config_.flops_initial == FlopsInitial::Random) {
            rng_seed = config_.flops_initial_seed.value_or(std::random_device{}());
            std::mt19937_64 rng(rng_seed);
            for (const auto& [qnode, flop] : flop_q_nodes_) {
                int w = flop->type.type.width;
                uint64_t mask = (w == 64) ? ~0ULL : (1ULL << w) - 1;
                values_[qnode] = static_cast<int64_t>(rng() & mask);
            }
        } else {
            for (const auto& [qnode, flop] : flop_q_nodes_) {
                int w = flop->type.type.width;
                uint64_t mask = (w == 64) ? ~0ULL : (1ULL << w) - 1;
                values_[qnode] = (config_.flops_initial == FlopsInitial::AllOnes)
                    ? static_cast<int64_t>(mask) : 0;
            }
        }
    }

    // 3. Set async input values from first event in their timeline (must be at time 0)
    std::map<std::string, int64_t> async_prev;
    for (const auto& name : async_inputs_) {
        // Find the first event for this signal
        bool found = false;
        for (const auto& evt : timeline_) {
            if (evt.signal_name == name) {
                if (evt.time != 0) {
                    throw CompilerError(std::format(
                        "Simulator: async input '{}' first event is at time {} (must be 0)",
                        name, evt.time));
                }
                async_prev[name] = evt.value;
                auto it = module_.dfg->inputs.find(name);
                if (it != module_.dfg->inputs.end()) {
                    values_[it->second] = evt.value;
                }
                found = true;
                break;
            }
        }
        if (!found) {
            throw CompilerError(std::format(
                "Simulator: async input '{}' has no events in timeline", name));
        }
    }

    // 4. Set sync input values from first line of their files
    for (const auto& [name, data] : sync_input_data_) {
        auto it = module_.dfg->inputs.find(name);
        if (it != module_.dfg->inputs.end()) {
            values_[it->second] = data[0];
        }
    }

    // 5. If any reset is asserted at time 0 (level check), apply it
    for (const auto& [rst_name, flops] : flops_by_reset_) {
        int64_t rst_val = async_prev[rst_name];
        for (const auto* flop : flops) {
            // POSEDGE reset is active when signal is 1,
            // NEGEDGE reset is active when signal is 0.
            bool asserted = (flop->reset->edge == POSEDGE && rst_val == 1) ||
                            (flop->reset->edge == NEGEDGE && rst_val == 0);
            if (asserted && flop->reset_value.has_value()) {
                std::string qname = flop->name + ".q";
                auto qit = module_.dfg->signals.find(qname);
                if (qit != module_.dfg->signals.end()) {
                    values_[qit->second] = flop->reset_value.value();
                }
            }
        }
    }

    // 6. Evaluate all combinational logic
    evaluateCombinational();

    // VCD: trace initial state at time 0
    updateVcdValues(vcd_out, 0, async_prev);
    updateVcdValuesFlat(vcd_flat_out, 0, async_prev);

    std::cout << "Simulator: initialization complete, processing "
              << timeline_.size() << " async events" << std::endl;

    // Log outputs before first clock edge
    recordOutputs();

    // === Main loop: process timeline in time-batches ===

    size_t idx = 0;
    while (idx < timeline_.size()) {
        int64_t batch_time = timeline_[idx].time;

        // Collect all events at this time
        std::vector<const AsyncEvent*> batch;
        while (idx < timeline_.size() && timeline_[idx].time == batch_time) {
            batch.push_back(&timeline_[idx]);
            idx++;
        }

        // Apply new async values and detect edges
        std::map<std::string, int64_t> new_async;
        for (const auto* evt : batch) {
            new_async[evt->signal_name] = evt->value;
        }

        // Detect active edges
        bool any_clock_edge = false;
        bool any_reset_edge = false;
        std::set<std::string> active_clock_edges;
        std::set<std::string> active_reset_edges;

        for (const auto& [name, new_val] : new_async) {
            int64_t old_val = async_prev[name];

            // Update the input node value
            auto it = module_.dfg->inputs.find(name);
            if (it != module_.dfg->inputs.end()) {
                values_[it->second] = new_val;
            }

            // Check for edges
            bool posedge = (old_val == 0 && new_val == 1);
            bool negedge = (old_val == 1 && new_val == 0);

            // Is this a clock?
            if (flops_by_clock_.count(name)) {
                // Check edge type matches what the flops expect
                const auto& flops = flops_by_clock_[name];
                for (const auto* flop : flops) {
                    if ((flop->clock.edge == POSEDGE && posedge) ||
                        (flop->clock.edge == NEGEDGE && negedge)) {
                        active_clock_edges.insert(name);
                        any_clock_edge = true;
                    }
                }
            }

            // Is this a reset?
            if (flops_by_reset_.count(name)) {
                const auto& flops = flops_by_reset_[name];
                for (const auto* flop : flops) {
                    if ((flop->reset->edge == POSEDGE && posedge) ||
                        (flop->reset->edge == NEGEDGE && negedge)) {
                        active_reset_edges.insert(name);
                        any_reset_edge = true;
                    }
                }
            }

            async_prev[name] = new_val;
        }

        // 3. On reset active edge: set affected flop .q = reset_value
        if (any_reset_edge) {
            for (const auto& rst_name : active_reset_edges) {
                for (const auto* flop : flops_by_reset_[rst_name]) {
                    std::string qname = flop->name + ".q";
                    auto qit = module_.dfg->signals.find(qname);
                    if (qit != module_.dfg->signals.end() && flop->reset_value.has_value()) {
                        values_[qit->second] = flop->reset_value.value();
                    }
                }
            }
        }

        // 4. On clock active edge: set affected flop .q = .d value, advance sync inputs
        if (any_clock_edge) {
            for (const auto& clk_name : active_clock_edges) {
                for (const auto* flop : flops_by_clock_[clk_name]) {
                    std::string qname = flop->name + ".q";
                    std::string dname = flop->name + ".d";
                    auto qit = module_.dfg->signals.find(qname);
                    auto dit = module_.dfg->signals.find(dname);
                    if (qit != module_.dfg->signals.end() && dit != module_.dfg->signals.end()) {
                        values_[qit->second] = values_.at(dit->second);
                    }
                }
            }
            advanceSyncInputs();
        }

        // 5. Re-evaluate combinational logic
        evaluateCombinational();

        // 6. VCD: trace all values at every time step
        updateVcdValues(vcd_out, batch_time, async_prev);
        updateVcdValuesFlat(vcd_flat_out, batch_time, async_prev);

        // 7. Record output values only on clock edges (for text output)
        if (any_clock_edge) {
            recordOutputs();
        }
    }

    // Flush last pending VCD values at the final event time
    if (!timeline_.empty()) {
        vcd_top_->time_update_abs(vcd_out, std::chrono::nanoseconds{timeline_.back().time});
        vcd_flat_top_->time_update_abs(vcd_flat_out, std::chrono::nanoseconds{timeline_.back().time});
    }
    vcd_out.close();
    vcd_flat_out.close();

    // Write text output files
    writeOutputFiles();

    std::cout << "Simulator: simulation complete. "
              << recorded_values_.begin()->second.size() << " cycles recorded."
              << std::endl;
    if (config_.flops_initial == FlopsInitial::Random) {
        std::cout << "Simulator: flops-initial seed = " << rng_seed << std::endl;
    }
    std::cout << "Simulator: output written to '" << config_.output_dir << "/'" << std::endl;
    std::cout << "Simulator: VCD trace written to '" << vcd_path << "'" << std::endl;
    std::cout << "Simulator: flat VCD trace written to '" << vcd_flat_path << "'" << std::endl;
}

} // namespace custom_hdl
