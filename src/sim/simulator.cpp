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
#include <stdexcept>
#include <utility>


namespace mate {

namespace {

int nodeWidth(const DFGNode* node) {
    if (!node || !node->type.has_value() || node->type->width <= 0)
        throw CompilerError(std::format(
            "Simulator: node {} has no resolved type width (type_propagation incomplete?)",
            node ? node->str() : "<null>"),
            node);
    return node->type->width;
}

bool nodeSigned(const DFGNode* node) {
    return node && node->type.has_value() && node->type->isSigned();
}

SimValue simValueFromInt(int64_t value, const DFGNode* node) {
    return SimValue::fromI64(value, nodeWidth(node), nodeSigned(node));
}

SimValue simValueFromType(int64_t value, const Type& type) {
    if (type.width <= 0)
        throw CompilerError(std::format(
            "Simulator: Type has no width (type_propagation incomplete?)"));
    return SimValue::fromI64(value, type.width, type.isSigned());
}

SimValue boolValue(bool value) {
    return SimValue::fromU64(value ? 1 : 0, 1, false);
}

void assignFlopResetLeaves(ModuleInstance& root, const FlopInfo& flop, int64_t resetValue) {
    for (auto* qLeaf : flopQLeaves(flop)) {
        if (!qLeaf) continue;
        const Type& type = qLeaf->type.value_or(flop.type);
        root.values[qLeaf] = simValueFromType(resetValue, type);
    }
}

void copyFlopDToQLeaves(ModuleInstance& root, const FlopInfo& flop) {
    const auto& qLeaves = flopQLeaves(flop);
    const auto& dLeaves = flopDLeaves(flop);
    if (qLeaves.size() != dLeaves.size()) {
        throw CompilerError(std::format(
            "Simulator: flop '{}' has mismatched d/q leaf counts ({} vs {})",
            flop.name, dLeaves.size(), qLeaves.size()));
    }
    for (size_t i = 0; i < qLeaves.size(); ++i) {
        auto* qLeaf = qLeaves[i];
        auto* dLeaf = dLeaves[i];
        if (qLeaf && dLeaf) {
            root.values[qLeaf] = root.checkedGet(dLeaf);
        }
    }
}

SimValue widenForArithmetic(const SimValue& value, const DFGNode* result_node) {
    return value.resized(nodeWidth(result_node), value.isSigned());
}

bool useSignedCompare(const DFGNode* lhs, const DFGNode* rhs) {
    return nodeSigned(lhs) && nodeSigned(rhs);
}

bool isTopInputSource(const HierSignalRef& source) {
    return source.instance_path.elems.empty() && source.ns == SignalNamespace::Input;
}

bool isActiveLevel(const SimValue& value, edge_t active_edge) {
    return active_edge == POSEDGE
        ? (!value.isZero() && value.lowU64() == 1)
        : value.isZero();
}

} // namespace

// ============================================================================
// ModuleInstance
// ============================================================================

ModuleInstance::ModuleInstance(const std::string& name, const Module& mod, const MateIR& mate_ir)
    : instance_name(name), module_def(mod), ir(mate_ir)
{
    buildFlopMaps();
    buildTopology();
    initConsts();
}

// ============================================================================
// Bit mask helper
// ============================================================================

SimValue ModuleInstance::maskToWidth(const SimValue& val, const DFGNode* node) {
    if (val.isAggregate()) return val;
    if (!node->type.has_value() || node->type->width <= 0)
        throw CompilerError(std::format(
            "Simulator: node {} has no resolved type width for masking (type_propagation incomplete?)",
            node->str()),
            node);
    return val.resized(node->type->width, node->type->isSigned());
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
        DFGTraversal::forEachInput(node.get(), [&](size_t, const DFGOutput& input) {
            in_degree[node.get()]++;
            successors[input.node].push_back(node.get());
        });
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
    // After DFG inlining, flop binding pointers are valid in the flat top DFG.
    std::function<void(const Module&)> collect = [&](const Module& mod) {
        for (const auto& flop : mod.flops) {
            for (auto* qLeaf : flopQLeaves(flop)) {
                if (qLeaf) flop_q_nodes[qLeaf] = &flop;
            }
            if (flop.clock_domain == InvalidClockId) {
                throw CompilerError(std::format(
                    "Simulator: flop '{}' has no resolved clock domain", flop.name));
            }
            if (flop.clock_domain.value >= ir.clocks.size() ||
                    ir.clocks[flop.clock_domain.value].id != flop.clock_domain) {
                throw CompilerError(std::format(
                    "Simulator: flop '{}' has invalid ClockId {}",
                    flop.name, flop.clock_domain.value));
            }
            for (ResetId resetId : flop.reset_domains.ids) {
                if (resetId == InvalidResetId ||
                        resetId.value >= ir.resets.size() ||
                        ir.resets[resetId.value].id != resetId) {
                    throw CompilerError(std::format(
                        "Simulator: flop '{}' has invalid ResetId {}",
                        flop.name, resetId.value));
                }
            }
            if (!flop.reset_domains.empty() && !flop.reset_value.has_value()) {
                throw CompilerError(std::format(
                    "Simulator: flop '{}' has reset domains but no reset value",
                    flop.name));
            }
            CollectedFlop collected{
                .flop = &flop,
                .clock_domain = flop.clock_domain,
                .reset_domains = flop.reset_domains,
            };
            for (ResetId resetId : collected.reset_domains.ids) {
                flops_by_reset[resetId].push_back(collected);
            }
            flops_by_clock[collected.clock_domain].push_back(std::move(collected));
        }
        for (const auto& sub : mod.hierarchyInstantiation) {
            collect(sub);
        }
    };
    collect(module_def);
}

// ============================================================================
// Initialize constant node values
// ============================================================================

void ModuleInstance::initConsts() {
    for (const auto& node : module_def.dfg->nodes) {
        if (node->kind() == DFGOp::CONST) {
            values[node.get()] = simValueFromInt(node->constValue(), node.get());
        }
    }
}

// ============================================================================
// Initialize flop values (recursive)
// ============================================================================

void ModuleInstance::initFlops(FlopsInitial mode, std::mt19937_64& rng) {
    // flop_q_nodes already covers all flops from all submodules (built by buildFlopMaps)
    for (const auto& [qnode, flop] : flop_q_nodes) {
        const Type& type = qnode->type.value_or(flop->type);
        int w = type.width;
        if (mode == FlopsInitial::Random) {
            values[qnode] = SimValue::random(w, type.isSigned(), rng);
        } else if (mode == FlopsInitial::AllOnes) {
            values[qnode] = SimValue::ones(w, type.isSigned());
        } else {
            values[qnode] = simValueFromType(0, type);
        }
    }
}

// ============================================================================
// Node evaluation
// ============================================================================

SimValue ModuleInstance::checkedGet(const DFGNode* node, const DFGNode* context) const {
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

SimValue ModuleInstance::evaluateNode(const DFGNode* node) {
    auto getUnaryVal = [&]() -> SimValue {
        return checkedGet(node->unaryInputs().operand.node, node);
    };
    auto getBinaryVals = [&]() -> std::pair<SimValue, SimValue> {
        auto inputs = node->binaryInputs();
        return {checkedGet(inputs.lhs.node, node), checkedGet(inputs.rhs.node, node)};
    };

    switch (node->kind()) {
        case DFGOp::INPUT:
        case DFGOp::CONST:
            return checkedGet(node);

        case DFGOp::SIGNAL:
        case DFGOp::OUTPUT:
            if (auto driver = node->driver()) return checkedGet(driver->node, node);
            return checkedGet(node);

        case DFGOp::ADD: {
            auto [lhsRaw, rhsRaw] = getBinaryVals();
            SimValue lhs = widenForArithmetic(lhsRaw, node);
            SimValue rhs = widenForArithmetic(rhsRaw, node);
            return maskToWidth(lhs.add(rhs), node);
        }
        case DFGOp::SUB: {
            auto [lhsRaw, rhsRaw] = getBinaryVals();
            SimValue lhs = widenForArithmetic(lhsRaw, node);
            SimValue rhs = widenForArithmetic(rhsRaw, node);
            return maskToWidth(lhs.sub(rhs), node);
        }
        case DFGOp::MUL: {
            auto [lhsRaw, rhsRaw] = getBinaryVals();
            SimValue lhs = widenForArithmetic(lhsRaw, node);
            SimValue rhs = widenForArithmetic(rhsRaw, node);
            return maskToWidth(lhs.mul(rhs), node);
        }

        case DFGOp::EQ: {
            auto [lhs, rhs] = getBinaryVals();
            return boolValue(lhs.eq(rhs));
        }
        case DFGOp::LT: {
            auto inputs = node->binaryInputs();
            auto lhs = checkedGet(inputs.lhs.node, node);
            auto rhs = checkedGet(inputs.rhs.node, node);
            return boolValue(useSignedCompare(inputs.lhs.node, inputs.rhs.node)
                ? lhs.signedLt(rhs)
                : lhs.unsignedLt(rhs));
        }
        case DFGOp::LE: {
            auto inputs = node->binaryInputs();
            auto lhs = checkedGet(inputs.lhs.node, node);
            auto rhs = checkedGet(inputs.rhs.node, node);
            bool lt = useSignedCompare(inputs.lhs.node, inputs.rhs.node)
                ? lhs.signedLt(rhs)
                : lhs.unsignedLt(rhs);
            return boolValue(lt || lhs.eq(rhs));
        }
        case DFGOp::GT: {
            auto inputs = node->binaryInputs();
            auto lhs = checkedGet(inputs.lhs.node, node);
            auto rhs = checkedGet(inputs.rhs.node, node);
            return boolValue(useSignedCompare(inputs.lhs.node, inputs.rhs.node)
                ? rhs.signedLt(lhs)
                : rhs.unsignedLt(lhs));
        }
        case DFGOp::GE: {
            auto inputs = node->binaryInputs();
            auto lhs = checkedGet(inputs.lhs.node, node);
            auto rhs = checkedGet(inputs.rhs.node, node);
            bool lt = useSignedCompare(inputs.lhs.node, inputs.rhs.node)
                ? lhs.signedLt(rhs)
                : lhs.unsignedLt(rhs);
            return boolValue(!lt);
        }

        case DFGOp::SHL: {
            auto [lhs, rhs] = getBinaryVals();
            return lhs.shl(rhs.lowU64());
        }
        case DFGOp::ASR: {
            auto [lhs, rhs] = getBinaryVals();
            return lhs.shr(rhs.lowU64(), true);
        }

        case DFGOp::MUX:
        {
            int64_t selectorValue = static_cast<int64_t>(checkedGet(node->muxSelector().node, node).lowU64());
            int armIndex = node->muxArmIndexForValue(selectorValue);
            if (armIndex < 0) {
                throw CompilerError(
                    std::format("Simulator: MUX {} has no arm for selector value {}",
                        node->str(), selectorValue),
                    node);
            }
            return checkedGet(node->muxArmData(static_cast<size_t>(armIndex)).node, node);
        }

        case DFGOp::UNARY_NEGATE:  return getUnaryVal().negated();
        case DFGOp::BITWISE_NOT:   return getUnaryVal().bitwiseNot();
        case DFGOp::BITWISE_AND: {
            auto [lhs, rhs] = getBinaryVals();
            return lhs.bitwiseAnd(rhs);
        }
        case DFGOp::BITWISE_OR: {
            auto [lhs, rhs] = getBinaryVals();
            return lhs.bitwiseOr(rhs);
        }
        case DFGOp::BITWISE_XOR: {
            auto [lhs, rhs] = getBinaryVals();
            return lhs.bitwiseXor(rhs);
        }
        case DFGOp::BITWISE_XNOR: {
            auto [lhs, rhs] = getBinaryVals();
            return lhs.bitwiseXnor(rhs);
        }

        case DFGOp::REDUCTION_AND:
            return boolValue(getUnaryVal().reductionAnd());
        case DFGOp::REDUCTION_NAND:
            return boolValue(!getUnaryVal().reductionAnd());
        case DFGOp::REDUCTION_OR:
            return boolValue(getUnaryVal().reductionOr());
        case DFGOp::REDUCTION_NOR:
            return boolValue(!getUnaryVal().reductionOr());
        case DFGOp::REDUCTION_XOR:
            return boolValue(getUnaryVal().reductionXor());
        case DFGOp::REDUCTION_XNOR:
            return boolValue(!getUnaryVal().reductionXor());

        case DFGOp::SLICE: {
            // SLICE has CONST high/low. Dynamic indexing is lowered to MUX during elaboration.
            // Unpacked array access is lowered to scalar leaves or MUX-over-leaves;
            // aggregate SIGNAL nodes no longer appear in the live DFG.
            auto slice = node->sliceInputs();
            const DFGNode* source_node = slice.source.node;

            int64_t high = static_cast<int64_t>(checkedGet(slice.high.node, node).lowU64());
            int64_t low = static_cast<int64_t>(checkedGet(slice.low.node, node).lowU64());
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
                // Source is a flat bit-vector (no packed_dims): 0-indexed, bit_pos = low.
                // This is correct — CONCAT results and range-select SLICE results
                // are intentionally flat, with no explicit dimension descriptor.
                bit_pos = low;
            }

            return checkedGet(slice.source.node, node).slice(static_cast<int>(bit_pos + width - 1), static_cast<int>(bit_pos));
        }

        case DFGOp::CONCAT: {
            std::vector<SimValue> parts;
            parts.reserve(node->concatParts().size());
            for (const auto& part : node->concatParts()) {
                parts.push_back(checkedGet(part.node, node));
            }
            return SimValue::concat(parts);
        }
    }

    throw CompilerError(std::format("Simulator: unhandled op {}", to_string(node->kind())), node);
}

// ============================================================================
// Combinational evaluation — single topo-ordered pass (flat DAG, no cycles)
// ============================================================================

void ModuleInstance::evaluateCombinational() {
    for (const DFGNode* node : topo_order) {
        if (node->kind() == DFGOp::INPUT || node->kind() == DFGOp::CONST) continue;
        if (flop_q_nodes.count(node)) continue;
        SimValue val = maskToWidth(evaluateNode(node), node);
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

void Simulator::buildTopInputDomainMaps() {
    clock_domains_by_top_input_.clear();
    reset_domains_by_top_input_.clear();
    reset_top_input_by_id_.clear();

    for (const auto& clock : ir_.clocks) {
        if (!isTopInputSource(clock.source)) {
            throw CompilerError(std::format(
                "Simulator: clock domain '{}' has unsupported non-top-input source",
                clock.display_name));
        }
        clock_domains_by_top_input_[clock.source.name].push_back(clock.id);
    }

    for (const auto& reset : ir_.resets) {
        if (!isTopInputSource(reset.source)) {
            throw CompilerError(std::format(
                "Simulator: reset domain '{}' has unsupported non-top-input source",
                reset.display_name));
        }
        reset_domains_by_top_input_[reset.source.name].push_back(reset.id);
        reset_top_input_by_id_[reset.id] = reset.source.name;
    }
}

void Simulator::buildTimeline() {
    // Determine which inputs are async (clocks, resets, and async data) from resolved input types
    for (const auto& [name, input] : module_.inputs) {
        if (std::holds_alternative<ClockSignal>(input.sync_type) ||
            std::holds_alternative<ResetSignal>(input.sync_type) ||
            std::holds_alternative<AsyncSignal>(input.sync_type)) {
            async_inputs_.insert(name);
        }
    }

    // Map each sync input to its clock domain.
    for (const auto& [name, input] : module_.inputs) {
        if (async_inputs_.count(name)) continue;
        const auto* sync = std::get_if<SyncSignal>(&input.sync_type);
        if (!sync) {
            if (std::holds_alternative<StaticSignal>(input.sync_type)) {
                throw CompilerError(std::format(
                    "Simulator: top-level input '{}' cannot be static", name));
            }
            throw CompilerError(std::format(
                "Simulator: input '{}' has unsupported sync type for synchronous input",
                name));
        }
        ClockId id = sync->clock_domain;
        if (id == InvalidClockId || id.value >= ir_.clocks.size() ||
                ir_.clocks[id.value].id != id) {
            throw CompilerError(std::format(
                "Simulator: sync input '{}' references invalid ClockId {}",
                name, id.value));
        }
        sync_input_clock_[name] = id;
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
            std::string value_token;
            if (!(iss >> time_token >> value_token)) {
                throw CompilerError(std::format(
                    "Simulator: bad line in async file '{}': {}", path, line));
            }
            SimValue parsedValue;
            try {
                auto inputIt = module_.inputs.find(name);
                if (inputIt == module_.inputs.end()) {
                    throw CompilerError(std::format(
                        "Simulator: unknown async input '{}'", name));
                }
                const auto& input = inputIt->second;
                if (input.type.width <= 0)
                    throw CompilerError(std::format(
                        "Simulator: async input '{}' has no resolved type width", name));
                parsedValue = SimValue::fromHexString(
                    value_token,
                    input.type.width,
                    input.type.isSigned());
            } catch (const std::invalid_argument&) {
                throw CompilerError(std::format(
                    "Simulator: async file '{}' has bad value "
                    "(expected leading token like 0x1a2b, optional trailing debug text): {}",
                    path, line));
            }
            int64_t time = parseTimeWithUnit(time_token, path, line);
            timeline_.push_back({time, name, std::move(parsedValue)});
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
        if (!std::holds_alternative<SyncSignal>(input.sync_type)) continue;
        // (variable 'name' used by the rest of the loop body below)

        std::string path = config_.inputs_dir + "/" + name + ".txt";
        std::ifstream file(path);
        if (!file.is_open()) {
            throw CompilerError(std::format(
                "Simulator: cannot open sync input file '{}'", path));
        }

        std::vector<SimValue> values;
        std::string line;
        while (std::getline(file, line)) {
            if (line.empty() || line[0] == '#') continue;
            std::istringstream iss(line);
            std::string hex_text;
            if (!(iss >> hex_text)) {
                throw CompilerError(std::format(
                    "Simulator: sync file '{}' has unparseable line: {}", path, line));
            }
            try {
                if (input.type.width <= 0)
                    throw CompilerError(std::format(
                        "Simulator: sync input '{}' has no resolved type width", name));
                values.push_back(SimValue::fromHexString(
                    hex_text,
                    input.type.width,
                    input.type.isSigned()));
            } catch (const std::invalid_argument&) {
                throw CompilerError(std::format(
                    "Simulator: sync file '{}' has bad hex value "
                    "(expected leading token like 0x1a2b, optional trailing debug text): {}",
                    path, line));
            }
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

void Simulator::advanceSyncInputs(const std::set<ClockId>& active_clocks) {
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

bool Simulator::resetDomainActive(ResetId id) const {
    auto sourceIt = reset_top_input_by_id_.find(id);
    if (sourceIt == reset_top_input_by_id_.end()) {
        throw CompilerError(std::format(
            "Simulator: reset domain {} has no top-level input source", id.value));
    }
    auto valueIt = root_->async_values.find(sourceIt->second);
    if (valueIt == root_->async_values.end()) {
        throw CompilerError(std::format(
            "Simulator: reset input '{}' has no runtime value", sourceIt->second));
    }
    return isActiveLevel(valueIt->second, ir_.resets[id.value].active_edge);
}

std::set<ResetId> Simulator::activeResetDomains() const {
    std::set<ResetId> active;
    for (const auto& reset : ir_.resets) {
        if (resetDomainActive(reset.id)) {
            active.insert(reset.id);
        }
    }
    return active;
}

// ============================================================================
// Output recording
// ============================================================================

void Simulator::recordOutputs() {
    for (const auto& [_, output] : module_.outputs) {
        for (const auto& leaf : signalLeafRefs(output)) {
            if (!leaf.node) continue;
            recorded_values_[leaf.leaf_name].push_back(root_->checkedGet(leaf.node));
        }
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

Simulator::Simulator(const MateIR& ir, const SimConfig& config)
    : ir_(ir), module_(ir.top), config_(config)
{
    if (!module_.dfg) {
        throw CompilerError("Simulator: module has no DFG");
    }

    buildTopInputDomainMaps();

    // Create the root module instance (recursively creates children)
    root_ = std::make_unique<ModuleInstance>(module_.name, module_, ir_);

    buildTimeline();
    loadSyncInputs();
}

// ============================================================================
// Main simulation loop
// ============================================================================

void Simulator::run() {
    std::cout << "Simulator: starting simulation for module '" << module_.name << "'" << std::endl;

    // === VCD Setup ===
    vcd_ = std::make_unique<VcdWriter>(ir_, config_.output_dir);

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
    std::map<std::string, SimValue> async_prev;
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
    for (ResetId reset_id : activeResetDomains()) {
        auto flopIt = root_->flops_by_reset.find(reset_id);
        if (flopIt == root_->flops_by_reset.end()) continue;
        for (const auto& collected : flopIt->second) {
            const auto* flop = collected.flop;
            if (flop->reset_value.has_value()) {
                assignFlopResetLeaves(*root_, *flop, flop->reset_value.value());
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

        // Deduplicate: keep the last event per signal at this timestamp.
        std::map<std::string, SimValue> new_async;
        for (const auto* evt : batch) {
            new_async[evt->signal_name] = evt->value;
        }

        if (batch_time != 0 && batch.size() > 1) {
            throw CompilerError(std::format(
                "Simulator: multiple async events share timestamp {} ps; this is unsupported",
                batch_time));
        }

        std::set<ClockId> active_edge_clocks;
        std::set<ResetId> active_edge_resets;

        for (const auto& [name, new_val] : new_async) {
            const SimValue& old_val = async_prev[name];

            root_->async_values[name] = new_val;
            if (auto* inputNode = module_.dfg->getInputNode("", name)) {
                root_->values[inputNode] = new_val;
            }

            if (!old_val.eq(new_val)) {
                bool posedge = old_val.isZero() && !new_val.isZero() && new_val.lowU64() == 1;
                bool negedge = !old_val.isZero() && old_val.lowU64() == 1 && new_val.isZero();
                if (auto clock_it = clock_domains_by_top_input_.find(name);
                    clock_it != clock_domains_by_top_input_.end()) {
                    for (ClockId id : clock_it->second) {
                        const auto& clock = ir_.clocks[id.value];
                        if ((clock.edge == POSEDGE && posedge) ||
                            (clock.edge == NEGEDGE && negedge)) {
                            active_edge_clocks.insert(id);
                        }
                    }
                }
                if (auto reset_it = reset_domains_by_top_input_.find(name);
                    reset_it != reset_domains_by_top_input_.end()) {
                    for (ResetId id : reset_it->second) {
                        const auto& reset = ir_.resets[id.value];
                        if ((reset.active_edge == POSEDGE && posedge) ||
                            (reset.active_edge == NEGEDGE && negedge)) {
                            active_edge_resets.insert(id);
                        }
                    }
                }
            }

            async_prev[name] = new_val;
        }

        for (ResetId reset_id : active_edge_resets) {
            for (const auto& collected : root_->flops_by_reset[reset_id]) {
                const auto* flop = collected.flop;
                if (flop->reset_value.has_value()) {
                    assignFlopResetLeaves(*root_, *flop, flop->reset_value.value());
                }
            }
        }

        for (ClockId clock_id : active_edge_clocks) {
            for (const auto& collected : root_->flops_by_clock[clock_id]) {
                const auto* flop = collected.flop;
                bool reset_active = false;
                for (ResetId reset_id : collected.reset_domains.ids) {
                    if (resetDomainActive(reset_id)) {
                        reset_active = true;
                        break;
                    }
                }
                if (reset_active) continue;
                copyFlopDToQLeaves(*root_, *flop);
            }
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
    std::cout << "Simulator: grouped VCD trace written to '" << vcd_->grouped_path() << "'" << std::endl;
    std::cout << "Simulator: raw VCD trace written to '" << vcd_->raw_path() << "'" << std::endl;
}

} // namespace mate
