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
// ModuleInstance
// ============================================================================

ModuleInstance::ModuleInstance(const std::string& name, const ResolvedModule& mod)
    : instance_name(name), module_def(mod)
{
    buildFlopMaps();
    buildTopology();
    initConsts();

    // Recursively create child instances for MODULE nodes in DFG
    for (const auto& node : module_def.dfg->nodes) {
        if (node->op == DFGOp::MODULE) {
            const std::string& moduleType = std::get<std::string>(node->data);
            const std::string& instName = node->name;

            // Find the resolved submodule definition
            const ResolvedModule* subDef = nullptr;
            for (const auto& sub : module_def.hierarchyInstantiation) {
                if (sub.name == moduleType) {
                    subDef = &sub;
                    break;
                }
            }
            if (!subDef) {
                throw CompilerError(std::format(
                    "Simulator: MODULE node '{}' references type '{}' "
                    "not found in hierarchyInstantiation",
                    instName, moduleType), node.get());
            }

            children[node.get()] = std::make_unique<ModuleInstance>(instName, *subDef);
        }

        // Width check
        if (node->type.has_value() && node->type->width > 64) {
            throw CompilerError(std::format(
                "Simulator: node '{}' has width {} (max 64 supported)",
                node->str(), node->type->width), node.get());
        }
    }
}

// ============================================================================
// Bit mask helper
// ============================================================================

int64_t ModuleInstance::maskToWidth(int64_t val, const DFGNode* node) {
    if (!node->type.has_value() || node->type->width <= 0)
        return val;

    int w = node->type->width;
    uint64_t mask = (w == 64) ? ~0ULL : (1ULL << w) - 1;
    val &= static_cast<int64_t>(mask);

    if (node->type->isSigned() && w < 64 && (val & (1LL << (w - 1)))) {
        val |= ~static_cast<int64_t>(mask);
    }
    return val;
}

// ============================================================================
// Topological sort (Kahn's algorithm)
// ============================================================================

void ModuleInstance::buildTopology() {
    const auto& nodes = module_def.dfg->nodes;

    std::map<const DFGNode*, int> in_degree;
    std::map<const DFGNode*, std::vector<const DFGNode*>> successors;

    for (const auto& node : nodes) {
        in_degree[node.get()] = 0;
    }

    for (const auto& node : nodes) {
        for (const auto& input : node->in) {
            // Skip edges from MODULE source nodes — MODULE nodes are multi-output
            // and can create false cycles in the topo sort. The fixpoint loop in
            // evaluateCombinational handles convergence across MODULE boundaries.
            if (input.node->op == DFGOp::MODULE) continue;
            in_degree[node.get()]++;
            successors[input.node].push_back(node.get());
        }
    }

    std::queue<const DFGNode*> q;
    for (const auto& [node, deg] : in_degree) {
        if (deg == 0) q.push(node);
    }

    topo_order.clear();
    while (!q.empty()) {
        const DFGNode* curr = q.front();
        q.pop();
        topo_order.push_back(curr);

        for (const DFGNode* succ : successors[curr]) {
            if (--in_degree[succ] == 0) {
                q.push(succ);
            }
        }
    }

    if (topo_order.size() != nodes.size()) {
        throw CompilerError(std::format(
            "Simulator: topological sort failed — {} of {} nodes sorted (cycle in DFG?)",
            topo_order.size(), nodes.size()));
    }
}

// ============================================================================
// Build flop lookup maps
// ============================================================================

void ModuleInstance::buildFlopMaps() {
    for (const auto& flop : module_def.flops) {
        // Build flop .q node -> FlopInfo map
        std::string qname = flop.name + ".q";
        if (auto it = module_def.dfg->signals.find(qname); it != module_def.dfg->signals.end()) {
            flop_q_nodes[it->second] = &flop;
        }

        flops_by_clock[flop.clock.name].push_back(&flop);
        async_input_names.insert(flop.clock.name);

        if (flop.reset.has_value()) {
            flops_by_reset[flop.reset->name].push_back(&flop);
            async_input_names.insert(flop.reset->name);
        }
    }
}

// ============================================================================
// Initialize constant node values
// ============================================================================

void ModuleInstance::initConsts() {
    for (const auto& node : module_def.dfg->nodes) {
        if (node->op == DFGOp::CONST) {
            values[node.get()] = std::get<int64_t>(node->data);
        }
        // Initialize MODULE output values to 0 so consumers evaluated before
        // the MODULE node in the first fixpoint iteration don't crash.
        if (node->op == DFGOp::MODULE) {
            for (int p = 0; p < node->num_outputs(); ++p) {
                module_output_values[{node.get(), p}] = 0;
            }
        }
    }
}

// ============================================================================
// Initialize flop values (recursive)
// ============================================================================

void ModuleInstance::initFlops(FlopsInitial mode, std::mt19937_64& rng) {
    for (const auto& [qnode, flop] : flop_q_nodes) {
        int w = flop->type.type.width;
        uint64_t mask = (w == 64) ? ~0ULL : (1ULL << w) - 1;
        if (mode == FlopsInitial::Random) {
            values[qnode] = static_cast<int64_t>(rng() & mask);
        } else if (mode == FlopsInitial::AllOnes) {
            values[qnode] = static_cast<int64_t>(mask);
        } else {
            values[qnode] = 0;
        }
    }

    for (auto& [moduleNode, child] : children) {
        child->initFlops(mode, rng);
    }
}

// ============================================================================
// Get input value (handles MODULE multi-output sources)
// ============================================================================

int64_t ModuleInstance::getInputValue(const DFGOutput& input) const {
    if (input.node->op == DFGOp::MODULE) {
        return module_output_values.at({input.node, input.port});
    }
    return values.at(input.node);
}

// ============================================================================
// Async event processing (edge detection + d->q / reset)
// ============================================================================

void ModuleInstance::setAsyncEvent(const std::string& signalName, int64_t newValue) {
    int64_t oldValue = 0;
    if (auto it = async_values.find(signalName); it != async_values.end()) {
        oldValue = it->second;
    }

    // Update stored value and INPUT node
    async_values[signalName] = newValue;
    auto inputIt = module_def.dfg->inputs.find(signalName);
    if (inputIt != module_def.dfg->inputs.end()) {
        values[inputIt->second] = newValue;
    }

    // Detect edges
    bool posedge = (oldValue == 0 && newValue == 1);
    bool negedge = (oldValue == 1 && newValue == 0);

    // Clock edges -> d->q propagation
    if (auto it = flops_by_clock.find(signalName); it != flops_by_clock.end()) {
        for (const auto* flop : it->second) {
            if ((flop->clock.edge == POSEDGE && posedge) ||
                (flop->clock.edge == NEGEDGE && negedge)) {
                auto qit = module_def.dfg->signals.find(flop->name + ".q");
                auto dit = module_def.dfg->signals.find(flop->name + ".d");
                if (qit != module_def.dfg->signals.end() &&
                    dit != module_def.dfg->signals.end()) {
                    values[qit->second] = values.at(dit->second);
                }
            }
        }
    }

    // Reset edges -> apply reset value
    if (auto it = flops_by_reset.find(signalName); it != flops_by_reset.end()) {
        for (const auto* flop : it->second) {
            if ((flop->reset->edge == POSEDGE && posedge) ||
                (flop->reset->edge == NEGEDGE && negedge)) {
                if (flop->reset_value.has_value()) {
                    auto qit = module_def.dfg->signals.find(flop->name + ".q");
                    if (qit != module_def.dfg->signals.end()) {
                        values[qit->second] = flop->reset_value.value();
                    }
                }
            }
        }
    }
}

// ============================================================================
// Node evaluation
// ============================================================================

int64_t ModuleInstance::evaluateNode(const DFGNode* node) {
    auto getVal = [&](int idx) -> int64_t {
        return getInputValue(node->in[idx]);
    };

    switch (node->op) {
        case DFGOp::INPUT:
        case DFGOp::CONST:
            return values.at(node);

        case DFGOp::SIGNAL:
        case DFGOp::OUTPUT:
            if (node->in.empty()) return values.at(node);
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
        case DFGOp::ASR:  return getVal(0) >> getVal(1);

        case DFGOp::MUX:
            return getVal(0) ? getVal(1) : getVal(2);

        case DFGOp::MUX_N: {
            int n = static_cast<int>(node->in.size()) / 2;
            for (int i = 0; i < n; i++) {
                if (getInputValue(node->in[i]) != 0) {
                    return getInputValue(node->in[n + i]);
                }
            }
            return getInputValue(node->in[2 * n - 1]);
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

            if (source_node->type.has_value() && !source_node->type->unpacked_dims.empty()) {
                int64_t index = getVal(1);
                int64_t n = static_cast<int64_t>(source_node->in.size());
                index = ((index % n) + n) % n;
                return getInputValue(source_node->in[index]);
            }

            int64_t high = getVal(1);
            int64_t low = getVal(2);
            int64_t source_val = getVal(0);
            int64_t width = high - low + 1;

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
            uint64_t result = 0;
            for (size_t i = 0; i < node->in.size(); ++i) {
                const DFGNode* inputNode = node->in[i].node;
                int w = inputNode->type.has_value() ? inputNode->type->width : 64;
                uint64_t mask = (w >= 64) ? ~0ULL : (1ULL << w) - 1;
                result = (result << w) | (static_cast<uint64_t>(getInputValue(node->in[i])) & mask);
            }
            return static_cast<int64_t>(result);
        }

        case DFGOp::CONCAT_ALIGN:
            throw CompilerError("Simulator: CONCAT_ALIGN should have been cleaned up by concat_cleanup pass", node);

        case DFGOp::MODULE:
            throw CompilerError("Simulator: MODULE nodes should be handled by evaluateModuleNode, not evaluateNode", node);
    }

    throw CompilerError(std::format("Simulator: unhandled op {}", to_string(node->op)), node);
}

// ============================================================================
// Evaluate a MODULE node: push inputs, handle async, evaluate child, pull outputs
// ============================================================================

bool ModuleInstance::evaluateModuleNode(const DFGNode* moduleNode) {
    auto childIt = children.find(moduleNode);
    if (childIt == children.end()) {
        throw CompilerError(std::format(
            "Simulator: no child instance for MODULE node '{}'",
            moduleNode->name), moduleNode);
    }
    ModuleInstance& child = *childIt->second;

    bool inputs_changed = false;

    // Push parent values into child's INPUT nodes
    for (size_t i = 0; i < moduleNode->in.size(); ++i) {
        const std::string& portName = moduleNode->input_names[i];
        int64_t parentVal = getInputValue(moduleNode->in[i]);

        auto inputIt = child.module_def.dfg->inputs.find(portName);
        if (inputIt == child.module_def.dfg->inputs.end()) continue;

        // Check if value changed
        auto oldIt = child.values.find(inputIt->second);
        if (oldIt == child.values.end() || oldIt->second != parentVal) {
            inputs_changed = true;

            // If this is an async input in the child, use setAsyncEvent for edge detection
            if (child.async_input_names.count(portName)) {
                child.setAsyncEvent(portName, parentVal);
            } else {
                child.values[inputIt->second] = parentVal;
            }
        }
    }

    if (inputs_changed) {
        // Evaluate child's combinational logic
        child.evaluateCombinational();

        // Pull child OUTPUT values into parent's module_output_values
        for (int p = 0; p < static_cast<int>(moduleNode->output_names.size()); ++p) {
            const std::string& outName = moduleNode->output_names[p];
            auto outIt = child.module_def.dfg->outputs.find(outName);
            if (outIt != child.module_def.dfg->outputs.end()) {
                module_output_values[{moduleNode, p}] = child.values.at(outIt->second);
            }
        }
    }

    return inputs_changed;
}

// ============================================================================
// Combinational evaluation with fixpoint
// ============================================================================

void ModuleInstance::evaluateCombinational() {
    constexpr int MAX_ITER = 100;

    for (int iter = 0; iter < MAX_ITER; ++iter) {
        bool any_inputs_changed = false;

        for (const DFGNode* node : topo_order) {
            if (node->op == DFGOp::INPUT || node->op == DFGOp::CONST) continue;
            if (flop_q_nodes.count(node)) continue;

            if (node->op == DFGOp::MODULE) {
                any_inputs_changed |= evaluateModuleNode(node);
                continue;
            }

            int64_t val = maskToWidth(evaluateNode(node), node);
            values[node] = val;
        }

        if (!any_inputs_changed) break;

        if (iter == MAX_ITER - 1) {
            throw CompilerError(std::format(
                "Simulator: fixpoint did not converge after {} iterations in module '{}'",
                MAX_ITER, instance_name));
        }
    }
}

// ============================================================================
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
    // Determine which inputs are async (clocks and resets) from resolved input types
    for (const auto& input : module_.inputs) {
        if (input.type.kind == ResolvedTypeKind::Clock ||
            input.type.kind == ResolvedTypeKind::Reset) {
            async_inputs_.insert(input.name);
            if (input.type.kind == ResolvedTypeKind::Clock) {
                clock_inputs_.insert(input.name);
            }
        }
    }

    // Map each sync input to its clock domain and build per-clock active edge
    for (const auto& input : module_.inputs) {
        if (async_inputs_.count(input.name)) continue;
        if (input.clock_domain && input.clock_edge.has_value()) {
            sync_input_clock_[input.name] = input.clock_domain->name;
            clock_active_edge_[input.clock_domain->name] = *input.clock_edge;
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
            // Validate no extra tokens on the line
            std::string extra;
            if (iss >> extra) {
                throw CompilerError(std::format(
                    "Simulator: async file '{}' line has extra data "
                    "(expected '<time> <value>'): {}", path, line));
            }
            int64_t time = parseTimeWithUnit(time_token, path, line);
            timeline_.push_back({time, name, value});
        }
    }

    std::stable_sort(timeline_.begin(), timeline_.end(),
        [](const AsyncEvent& a, const AsyncEvent& b) { return a.time < b.time; });
}

// ============================================================================
// Load sync input files (one value per line)
// ============================================================================

void Simulator::loadSyncInputs() {
    for (const auto& [name, node] : module_.dfg->inputs) {
        if (async_inputs_.count(name)) continue;

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
            std::istringstream iss(line);
            int64_t val;
            if (!(iss >> val)) {
                throw CompilerError(std::format(
                    "Simulator: sync file '{}' has unparseable line: {}", path, line));
            }
            std::string extra;
            if (iss >> extra) {
                throw CompilerError(std::format(
                    "Simulator: sync file '{}' line has extra data "
                    "(expected single integer per line, got '{}'). "
                    "Is this an async (clock/reset) file?", path, line));
            }
            values.push_back(val);
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
// Sync input advancement
// ============================================================================

void Simulator::advanceSyncInputs(const std::set<std::string>& active_clocks) {
    for (auto& [name, pos] : sync_input_pos_) {
        // Only advance if this input's clock had its active edge
        auto clk_it = sync_input_clock_.find(name);
        if (clk_it == sync_input_clock_.end() || !active_clocks.count(clk_it->second))
            continue;

        if (pos + 1 < sync_input_data_[name].size()) {
            pos++;
        }
        auto it = module_.dfg->inputs.find(name);
        if (it != module_.dfg->inputs.end()) {
            root_->values[it->second] = sync_input_data_[name][pos];
        }
    }
}

// ============================================================================
// Output recording
// ============================================================================

void Simulator::recordOutputs() {
    for (const auto& [name, node] : module_.dfg->outputs) {
        recorded_values_[name].push_back(root_->values.at(node));
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
// VCD tracing helpers
// ============================================================================

static unsigned int getWidth(const DFGNode* node) {
    if (node->type.has_value() && node->type->width > 0)
        return static_cast<unsigned int>(node->type->width);
    return 64;
}

static void elaborateWithWidth(vcd_tracer::module& mod, vcd_tracer::value_base& var,
                               std::string_view name, unsigned int width) {
    auto original_add_fn = mod.get_add_fn();
    auto override_fn = [original_add_fn, width](
            std::string_view var_name, std::string_view var_type,
            unsigned int /*bit_size*/, vcd_tracer::scope_fn::dumper_fn fn)
            -> vcd_tracer::value_context {
        return original_add_fn(var_name, var_type, width, fn);
    };
    var.elaborate(override_fn, name);
}

// Helper: detect flop .d/.q pairs, returning signal names to exclude and .q entries to trace
static void detectFlopPairs(const DFG& dfg,
                            std::set<std::string>& flop_signal_names,
                            std::map<std::string, const DFGNode*>& flop_q_entries) {
    for (const auto& [name, node] : dfg.signals) {
        if (name.size() > 2 && name.substr(name.size() - 2) == ".d") {
            std::string base = name.substr(0, name.size() - 2);
            auto qit = dfg.signals.find(base + ".q");
            if (qit != dfg.signals.end()) {
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
}

// ============================================================================
// Hierarchical VCD setup for a module instance (recursive)
// ============================================================================

void Simulator::setupVcdHierForInstance(ModuleInstance& inst, vcd_tracer::module& parent) {
    vcd_tracer::module instScope(parent, inst.instance_name);

    const auto& dfg = *inst.module_def.dfg;

    std::set<std::string> flop_signal_names;
    std::map<std::string, const DFGNode*> flop_q_entries;
    detectFlopPairs(dfg, flop_signal_names, flop_q_entries);

    {
        vcd_tracer::module inputs_mod(instScope, "inputs");
        vcd_tracer::module signals_mod(instScope, "signals");
        vcd_tracer::module flops_mod(instScope, "flops");
        vcd_tracer::module outputs_mod(instScope, "outputs");

        for (const auto& [name, node] : dfg.inputs) {
            unsigned int w = getWidth(node);
            auto v = std::make_unique<vcd_tracer::value<int64_t>>();
            elaborateWithWidth(inputs_mod, *v, name, w);
            v->set_runtime_bit_size(w);
            inst.vcd_values[node] = std::move(v);
        }

        for (const auto& [name, node] : dfg.signals) {
            if (flop_signal_names.count(name)) continue;
            unsigned int w = getWidth(node);
            auto v = std::make_unique<vcd_tracer::value<int64_t>>();
            elaborateWithWidth(signals_mod, *v, name, w);
            v->set_runtime_bit_size(w);
            inst.vcd_values[node] = std::move(v);
        }

        for (const auto& [base_name, qnode] : flop_q_entries) {
            unsigned int w = getWidth(qnode);
            auto v = std::make_unique<vcd_tracer::value<int64_t>>();
            elaborateWithWidth(flops_mod, *v, base_name, w);
            v->set_runtime_bit_size(w);
            inst.vcd_values[qnode] = std::move(v);
        }

        for (const auto& [name, node] : dfg.outputs) {
            unsigned int w = getWidth(node);
            auto v = std::make_unique<vcd_tracer::value<int64_t>>();
            elaborateWithWidth(outputs_mod, *v, name, w);
            v->set_runtime_bit_size(w);
            inst.vcd_values[node] = std::move(v);
        }
    }

    for (auto& [moduleNode, child] : inst.children) {
        setupVcdHierForInstance(*child, instScope);
    }
}

// ============================================================================
// Flat VCD setup for a module instance (recursive)
// ============================================================================

void Simulator::setupVcdFlatForInstance(ModuleInstance& inst, vcd_tracer::module& parent) {
    vcd_tracer::module instScope(parent, inst.instance_name);

    const auto& dfg = *inst.module_def.dfg;

    std::set<std::string> flop_signal_names;
    std::map<std::string, const DFGNode*> flop_q_entries;
    detectFlopPairs(dfg, flop_signal_names, flop_q_entries);

    for (const auto& [name, node] : dfg.inputs) {
        unsigned int w = getWidth(node);
        auto v = std::make_unique<vcd_tracer::value<int64_t>>();
        elaborateWithWidth(instScope, *v, name, w);
        v->set_runtime_bit_size(w);
        inst.vcd_flat_values[node] = std::move(v);
    }

    for (const auto& [name, node] : dfg.signals) {
        if (flop_signal_names.count(name)) continue;
        unsigned int w = getWidth(node);
        auto v = std::make_unique<vcd_tracer::value<int64_t>>();
        elaborateWithWidth(instScope, *v, name, w);
        v->set_runtime_bit_size(w);
        inst.vcd_flat_values[node] = std::move(v);
    }

    for (const auto& [base_name, qnode] : flop_q_entries) {
        unsigned int w = getWidth(qnode);
        auto v = std::make_unique<vcd_tracer::value<int64_t>>();
        elaborateWithWidth(instScope, *v, base_name, w);
        v->set_runtime_bit_size(w);
        inst.vcd_flat_values[qnode] = std::move(v);
    }

    for (const auto& [name, node] : dfg.outputs) {
        unsigned int w = getWidth(node);
        auto v = std::make_unique<vcd_tracer::value<int64_t>>();
        elaborateWithWidth(instScope, *v, name, w);
        v->set_runtime_bit_size(w);
        inst.vcd_flat_values[node] = std::move(v);
    }

    for (auto& [moduleNode, child] : inst.children) {
        setupVcdFlatForInstance(*child, instScope);
    }
}

// ============================================================================
// Update VCD values for a module instance (recursive)
// ============================================================================

void Simulator::updateVcdForInstance(ModuleInstance& inst) {
    for (auto& [node, vcd_val] : inst.vcd_values) {
        auto it = inst.values.find(node);
        if (it != inst.values.end()) {
            vcd_val->set(it->second);
        }
    }
    for (auto& [node, vcd_val] : inst.vcd_flat_values) {
        auto it = inst.values.find(node);
        if (it != inst.values.end()) {
            vcd_val->set(it->second);
        }
    }
    for (auto& [moduleNode, child] : inst.children) {
        updateVcdForInstance(*child);
    }
}

// ============================================================================
// VCD setup (top-level)
// ============================================================================

void Simulator::setupVcd(std::ofstream& vcd_out) {
    vcd_top_ = std::make_unique<vcd_tracer::top>(module_.name);

    std::set<std::string> flop_signal_names;
    std::map<std::string, const DFGNode*> flop_q_entries;
    detectFlopPairs(*module_.dfg, flop_signal_names, flop_q_entries);

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
            root_->vcd_values[node] = std::move(v);
        }

        for (const auto& name : async_inputs_) {
            if (module_.dfg->inputs.count(name)) continue;
            auto v = std::make_unique<vcd_tracer::value<int64_t>>();
            elaborateWithWidth(inputs_mod, *v, name, 1);
            v->set_runtime_bit_size(1);
            vcd_async_values_[name] = std::move(v);
        }

        for (const auto& [name, node] : module_.dfg->signals) {
            if (flop_signal_names.count(name)) continue;
            unsigned int w = getWidth(node);
            auto v = std::make_unique<vcd_tracer::value<int64_t>>();
            elaborateWithWidth(signals_mod, *v, name, w);
            v->set_runtime_bit_size(w);
            root_->vcd_values[node] = std::move(v);
        }

        for (const auto& [base_name, qnode] : flop_q_entries) {
            unsigned int w = getWidth(qnode);
            auto v = std::make_unique<vcd_tracer::value<int64_t>>();
            elaborateWithWidth(flops_mod, *v, base_name, w);
            v->set_runtime_bit_size(w);
            root_->vcd_values[qnode] = std::move(v);
        }

        for (const auto& [name, node] : module_.dfg->outputs) {
            unsigned int w = getWidth(node);
            auto v = std::make_unique<vcd_tracer::value<int64_t>>();
            elaborateWithWidth(outputs_mod, *v, name, w);
            v->set_runtime_bit_size(w);
            root_->vcd_values[node] = std::move(v);
        }

        elaborateParams(params_mod, vcd_params_);

        // Setup hierarchical VCD for child instances
        for (auto& [moduleNode, child] : root_->children) {
            setupVcdHierForInstance(*child, vcd_top_->root);
        }
    }

    vcd_top_->finalize_header(vcd_out,
                              std::chrono::system_clock::from_time_t(0));
}

void Simulator::setupVcdFlat(std::ofstream& vcd_out) {
    vcd_flat_top_ = std::make_unique<vcd_tracer::top>(module_.name);

    std::set<std::string> flop_signal_names;
    std::map<std::string, const DFGNode*> flop_q_entries;
    detectFlopPairs(*module_.dfg, flop_signal_names, flop_q_entries);

    // All signals go directly into the root module (no sub-scopes)
    for (const auto& [name, node] : module_.dfg->inputs) {
        unsigned int w = getWidth(node);
        auto v = std::make_unique<vcd_tracer::value<int64_t>>();
        elaborateWithWidth(vcd_flat_top_->root, *v, name, w);
        v->set_runtime_bit_size(w);
        root_->vcd_flat_values[node] = std::move(v);
    }

    for (const auto& name : async_inputs_) {
        if (module_.dfg->inputs.count(name)) continue;
        auto v = std::make_unique<vcd_tracer::value<int64_t>>();
        elaborateWithWidth(vcd_flat_top_->root, *v, name, 1);
        v->set_runtime_bit_size(1);
        vcd_flat_async_values_[name] = std::move(v);
    }

    for (const auto& [name, node] : module_.dfg->signals) {
        if (flop_signal_names.count(name)) continue;
        unsigned int w = getWidth(node);
        auto v = std::make_unique<vcd_tracer::value<int64_t>>();
        elaborateWithWidth(vcd_flat_top_->root, *v, name, w);
        v->set_runtime_bit_size(w);
        root_->vcd_flat_values[node] = std::move(v);
    }

    for (const auto& [base_name, qnode] : flop_q_entries) {
        unsigned int w = getWidth(qnode);
        auto v = std::make_unique<vcd_tracer::value<int64_t>>();
        elaborateWithWidth(vcd_flat_top_->root, *v, base_name, w);
        v->set_runtime_bit_size(w);
        root_->vcd_flat_values[qnode] = std::move(v);
    }

    for (const auto& [name, node] : module_.dfg->outputs) {
        unsigned int w = getWidth(node);
        auto v = std::make_unique<vcd_tracer::value<int64_t>>();
        elaborateWithWidth(vcd_flat_top_->root, *v, name, w);
        v->set_runtime_bit_size(w);
        root_->vcd_flat_values[node] = std::move(v);
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

    // Setup flat VCD for child instances
    for (auto& [moduleNode, child] : root_->children) {
        setupVcdFlatForInstance(*child, vcd_flat_top_->root);
    }

    vcd_flat_top_->finalize_header(vcd_out,
                                   std::chrono::system_clock::from_time_t(0));
}

void Simulator::updateVcdValuesFlat(std::ofstream& vcd_out, int64_t time_ns,
                                    const std::map<std::string, int64_t>& async_values) {
    vcd_flat_top_->time_update_abs(vcd_out, std::chrono::nanoseconds{time_ns});

    for (auto& [node, vcd_val] : root_->vcd_flat_values) {
        vcd_val->set(root_->values.at(node));
    }
    for (auto& [name, vcd_val] : vcd_flat_async_values_) {
        auto it = async_values.find(name);
        if (it != async_values.end()) {
            vcd_val->set(it->second);
        }
    }

    // Update child instances
    for (auto& [moduleNode, child] : root_->children) {
        updateVcdForInstance(*child);
    }
}

void Simulator::updateVcdValues(std::ofstream& vcd_out, int64_t time_ns,
                                const std::map<std::string, int64_t>& async_values) {
    vcd_top_->time_update_abs(vcd_out, std::chrono::nanoseconds{time_ns});

    for (auto& [node, vcd_val] : root_->vcd_values) {
        vcd_val->set(root_->values.at(node));
    }
    for (auto& [name, vcd_val] : vcd_async_values_) {
        auto it = async_values.find(name);
        if (it != async_values.end()) {
            vcd_val->set(it->second);
        }
    }

    // Update child instances
    for (auto& [moduleNode, child] : root_->children) {
        updateVcdForInstance(*child);
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

    // Create the root module instance (recursively creates children)
    root_ = std::make_unique<ModuleInstance>(module_.name, module_);

    buildTimeline();
    loadSyncInputs();
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

    // 1. Initialize flop .q values (recursive)
    uint64_t rng_seed = 0;
    {
        if (config_.flops_initial == FlopsInitial::Random) {
            rng_seed = config_.flops_initial_seed.value_or(std::random_device{}());
        }
        std::mt19937_64 rng(rng_seed);
        root_->initFlops(config_.flops_initial, rng);
    }

    // 2. Set async input values from first event in their timeline (must be at time 0)
    std::map<std::string, int64_t> async_prev;
    for (const auto& name : async_inputs_) {
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
                    root_->values[it->second] = evt.value;
                }
                // Also initialize the root's async_values for edge detection
                root_->async_values[name] = evt.value;
                found = true;
                break;
            }
        }
        if (!found) {
            throw CompilerError(std::format(
                "Simulator: async input '{}' has no events in timeline", name));
        }
    }

    // 3. Set sync input values from first line of their files
    for (const auto& [name, data] : sync_input_data_) {
        auto it = module_.dfg->inputs.find(name);
        if (it != module_.dfg->inputs.end()) {
            root_->values[it->second] = data[0];
        }
    }

    // 4. If any reset is asserted at time 0 (level check), apply it
    for (const auto& [rst_name, flops] : root_->flops_by_reset) {
        int64_t rst_val = async_prev[rst_name];
        for (const auto* flop : flops) {
            bool asserted = (flop->reset->edge == POSEDGE && rst_val == 1) ||
                            (flop->reset->edge == NEGEDGE && rst_val == 0);
            if (asserted && flop->reset_value.has_value()) {
                std::string qname = flop->name + ".q";
                auto qit = module_.dfg->signals.find(qname);
                if (qit != module_.dfg->signals.end()) {
                    root_->values[qit->second] = flop->reset_value.value();
                }
            }
        }
    }

    // 5. Evaluate all combinational logic (with fixpoint for hierarchy)
    root_->evaluateCombinational();

    // VCD: trace initial state at time 0
    updateVcdValues(vcd_out, 0, async_prev);
    updateVcdValuesFlat(vcd_flat_out, 0, async_prev);

    std::cout << "Simulator: initialization complete, processing "
              << timeline_.size() << " async events" << std::endl;

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

        // Track which clocks had their active edge in this batch
        std::set<std::string> active_edge_clocks;

        for (const auto& [name, new_val] : new_async) {
            int64_t old_val = async_prev[name];

            // Use setAsyncEvent on root for edge detection and d->q
            root_->setAsyncEvent(name, new_val);

            // Detect active edge on clock inputs
            if (clock_inputs_.count(name) && old_val != new_val) {
                bool is_posedge = (old_val == 0 && new_val == 1);
                bool is_negedge = (old_val == 1 && new_val == 0);
                auto edge_it = clock_active_edge_.find(name);
                if (edge_it != clock_active_edge_.end()) {
                    if ((edge_it->second == POSEDGE && is_posedge) ||
                        (edge_it->second == NEGEDGE && is_negedge)) {
                        active_edge_clocks.insert(name);
                    }
                }
            }

            async_prev[name] = new_val;
        }

        // On clock active edge: advance sync inputs
        if (!active_edge_clocks.empty()) {
            advanceSyncInputs(active_edge_clocks);
        }

        // Re-evaluate combinational logic (with fixpoint for hierarchy)
        root_->evaluateCombinational();

        // VCD: trace all values at every time step
        updateVcdValues(vcd_out, batch_time, async_prev);
        updateVcdValuesFlat(vcd_flat_out, batch_time, async_prev);

        // Record output values only on active clock edges (for text output)
        if (!active_edge_clocks.empty()) {
            recordOutputs();
        }
    }

    // Flush last pending VCD values
    if (!timeline_.empty()) {
        vcd_top_->time_update_abs(vcd_out, std::chrono::nanoseconds{timeline_.back().time});
        vcd_flat_top_->time_update_abs(vcd_flat_out, std::chrono::nanoseconds{timeline_.back().time});
    }
    vcd_out.close();
    vcd_flat_out.close();

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
