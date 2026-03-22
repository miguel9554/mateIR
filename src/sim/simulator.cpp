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

    // Width check on all nodes
    for (const auto& node : module_def.dfg->nodes) {
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
    // Collect flops from the entire hierarchy (all submodules, bottom-up).
    // After DFG inlining, d_node/q_node pointers are valid in the flat top DFG.
    //
    // translation: maps this module's async port names -> top-level signal names.
    // At the top level it is empty (identity). For each submodule it is composed
    // from the parent's translation and the submodule's asyncPortConnections.
    using NameMap = std::map<std::string, std::string>;
    auto translate = [](const std::string& name, const NameMap& map) -> const std::string& {
        auto it = map.find(name);
        return it != map.end() ? it->second : name;
    };

    std::function<void(const ResolvedModule&, const NameMap&)> collect =
        [&](const ResolvedModule& mod, const NameMap& translation) {
        for (const auto& flop : mod.flops) {
            if (flop.q_node) {
                flop_q_nodes[flop.q_node] = &flop;
            }
            if (!flop.clock.name.empty()) {
                const std::string& clk = translate(flop.clock.name, translation);
                flops_by_clock[clk].push_back(&flop);
                async_input_names.insert(clk);
            }
            if (flop.reset.has_value()) {
                const std::string& rst = translate(flop.reset->name, translation);
                flops_by_reset[rst].push_back(&flop);
                async_input_names.insert(rst);
            }
        }
        for (const auto& sub : mod.hierarchyInstantiation) {
            // Compose: sub port name -> top-level name
            NameMap composed;
            for (const auto& [port, parent_sig] : sub.asyncPortConnections) {
                composed[port] = translate(parent_sig, translation);
            }
            collect(sub, composed);
        }
    };
    collect(module_def, {});
}

// ============================================================================
// Initialize constant node values
// ============================================================================

void ModuleInstance::initConsts() {
    for (const auto& node : module_def.dfg->nodes) {
        if (node->op == DFGOp::CONST) {
            values[node.get()] = std::get<int64_t>(node->data);
        }
    }
}

// ============================================================================
// Initialize flop values (recursive)
// ============================================================================

void ModuleInstance::initFlops(FlopsInitial mode, std::mt19937_64& rng) {
    // flop_q_nodes already covers all flops from all submodules (built by buildFlopMaps)
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
    if (auto* inputNode = module_def.dfg->getInputNode("", signalName)) {
        values[inputNode] = newValue;
    }

    // Detect edges
    bool posedge = (oldValue == 0 && newValue == 1);
    bool negedge = (oldValue == 1 && newValue == 0);

    // Clock edges -> d->q propagation (skipped if async reset is currently asserted)
    if (auto it = flops_by_clock.find(signalName); it != flops_by_clock.end()) {
        for (const auto* flop : it->second) {
            if ((flop->clock.edge == POSEDGE && posedge) ||
                (flop->clock.edge == NEGEDGE && negedge)) {
                // Async reset has priority: skip d->q if reset is active
                if (flop->reset.has_value()) {
                    int64_t rst_val = 0;
                    if (auto rit = async_values.find(flop->reset->name); rit != async_values.end()) {
                        rst_val = rit->second;
                    }
                    bool rst_active = (flop->reset->edge == POSEDGE && rst_val == 1) ||
                                      (flop->reset->edge == NEGEDGE && rst_val == 0);
                    if (rst_active) continue;
                }
                if (flop->q_node && flop->d_node) {
                    values[flop->q_node] = checkedGet(flop->d_node);
                }
            }
        }
    }

    // Reset edges -> apply reset value
    if (auto it = flops_by_reset.find(signalName); it != flops_by_reset.end()) {
        for (const auto* flop : it->second) {
            if ((flop->reset->edge == POSEDGE && posedge) ||
                (flop->reset->edge == NEGEDGE && negedge)) {
                if (flop->reset_value.has_value() && flop->q_node) {
                    values[flop->q_node] = flop->reset_value.value();
                }
            }
        }
    }
}

// ============================================================================
// Node evaluation
// ============================================================================

int64_t ModuleInstance::checkedGet(const DFGNode* node, const DFGNode* context) const {
    auto it = values.find(node);
    if (it == values.end())
        throw CompilerError(std::format(
            "Simulator: node {} has no computed value{}",
            node->str(),
            context && context != node
                ? std::format(" (while evaluating {})", context->str())
                : ""),
            context ? context : node);
    return it->second;
}

int64_t ModuleInstance::evaluateNode(const DFGNode* node) {
    auto getVal = [&](int idx) -> int64_t {
        return checkedGet(node->in[idx].node, node);
    };

    switch (node->op) {
        case DFGOp::INPUT:
        case DFGOp::CONST:
            return checkedGet(node);

        case DFGOp::SIGNAL:
        case DFGOp::OUTPUT:
            if (node->in.empty()) return checkedGet(node);
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
                if (checkedGet(node->in[i].node, node) != 0) {
                    return checkedGet(node->in[n + i].node, node);
                }
            }
            return checkedGet(node->in[2 * n - 1].node, node);
        }

        case DFGOp::UNARY_PLUS:    return getVal(0);
        case DFGOp::UNARY_NEGATE:  return -getVal(0);
        case DFGOp::BITWISE_NOT:   return ~getVal(0);
        case DFGOp::LOGICAL_NOT:   return getVal(0) == 0 ? 1 : 0;
        case DFGOp::LOGICAL_AND:   return (getVal(0) != 0 && getVal(1) != 0) ? 1 : 0;

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
                return checkedGet(source_node->in[index].node, node);
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
                result = (result << w) | (static_cast<uint64_t>(checkedGet(inputNode, node)) & mask);
            }
            return static_cast<int64_t>(result);
        }

        case DFGOp::CONCAT_ALIGN:
            throw CompilerError("Simulator: CONCAT_ALIGN should have been cleaned up by concat_cleanup pass", node);

        case DFGOp::MODULE:
            throw CompilerError("Simulator: MODULE nodes should not exist after dfg_inline pass", node);
    }

    throw CompilerError(std::format("Simulator: unhandled op {}", to_string(node->op)), node);
}

// ============================================================================
// Combinational evaluation — single topo-ordered pass (flat DAG, no cycles)
// ============================================================================

void ModuleInstance::evaluateCombinational() {
    for (const DFGNode* node : topo_order) {
        if (node->op == DFGOp::INPUT || node->op == DFGOp::CONST) continue;
        if (flop_q_nodes.count(node)) continue;
        int64_t val = maskToWidth(evaluateNode(node), node);
        values[node] = val;
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
    // Determine which inputs are async (clocks, resets, and async data) from resolved input types
    for (const auto& [name, input] : module_.inputs) {
        if (input.sync_kind == SyncKind::Clock ||
            input.sync_kind == SyncKind::Reset ||
            input.sync_kind == SyncKind::Async) {
            async_inputs_.insert(name);
            if (input.sync_kind == SyncKind::Clock) {
                clock_inputs_.insert(name);
            }
        }
    }

    // Map each sync input to its clock domain and build per-clock active edge
    for (const auto& [name, input] : module_.inputs) {
        if (async_inputs_.count(name)) continue;
        if (input.clock_domain && input.clock_edge.has_value()) {
            sync_input_clock_[name] = input.clock_domain->name;
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
    for (const auto& [name, input] : module_.inputs) {
        if (input.sync_kind == SyncKind::Clock ||
            input.sync_kind == SyncKind::Reset ||
            input.sync_kind == SyncKind::Async) continue;
        // (variable 'name' used by the rest of the loop body below)

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
        if (auto* inputNode = module_.dfg->getInputNode("", name)) {
            root_->values[inputNode] = sync_input_data_[name][pos];
        }
    }
}

// ============================================================================
// Output recording
// ============================================================================

void Simulator::recordOutputs() {
    for (const auto& [name, node] : module_.dfg->getOutputsMap()) {
        recorded_values_[name].push_back(root_->checkedGet(node));
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
    vcd_ = std::make_unique<VcdWriter>(module_, config_.output_dir);

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
                if (auto* inputNode = module_.dfg->getInputNode("", name)) {
                    root_->values[inputNode] = evt.value;
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
        if (auto* inputNode = module_.dfg->getInputNode("", name)) {
            root_->values[inputNode] = data[0];
        }
    }

    // 4. If any reset is asserted at time 0 (level check), apply it
    for (const auto& [rst_name, flops] : root_->flops_by_reset) {
        int64_t rst_val = async_prev[rst_name];
        for (const auto* flop : flops) {
            bool asserted = (flop->reset->edge == POSEDGE && rst_val == 1) ||
                            (flop->reset->edge == NEGEDGE && rst_val == 0);
            if (asserted && flop->reset_value.has_value() && flop->q_node) {
                root_->values[flop->q_node] = flop->reset_value.value();
            }
        }
    }

    // 5. Evaluate all combinational logic (with fixpoint for hierarchy)
    root_->evaluateCombinational();

    // VCD: trace initial state at time 0
    vcd_->update(*root_, 0);

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
        vcd_->update(*root_, batch_time);

        // Record output values only on active clock edges (for text output)
        if (!active_edge_clocks.empty()) {
            recordOutputs();
        }
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
    std::cout << "Simulator: VCD trace written to '" << vcd_->hier_path() << "'" << std::endl;
    std::cout << "Simulator: flat VCD trace written to '" << vcd_->flat_path() << "'" << std::endl;
}

} // namespace custom_hdl
