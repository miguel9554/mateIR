#include "sim/runtime.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <format>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
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

// SV LRM 11.6.1: if either operand is unsigned, the expression is unsigned
// and both operands are zero-extended regardless of their individual types.
// We pass the other operand so we can enforce that rule.
SimValue widenForArithmetic(const SimValue& value,
                            const DFGNode* operand_node,
                            const DFGNode* other_node,
                            const DFGNode* result_node) {
    bool self_signed = (operand_node && operand_node->hasType())
                       ? operand_node->type->isSigned()
                       : value.isSigned();
    bool other_signed = (other_node && other_node->hasType())
                        ? other_node->type->isSigned()
                        : true;
    bool is_signed = self_signed && other_signed;
    return value.resized(nodeWidth(result_node), is_signed);
}

bool useSignedCompare(const DFGNode* lhs, const DFGNode* rhs) {
    return nodeSigned(lhs) && nodeSigned(rhs);
}

bool isActiveLevel(const SimValue& value, edge_t active_edge) {
    return active_edge == POSEDGE
        ? (!value.isZero() && value.lowU64() == 1)
        : value.isZero();
}

const char* edgeName(edge_t edge) {
    return edge == POSEDGE ? "posedge" : "negedge";
}

} // namespace

// ============================================================================
// MateIRRuntime
// ============================================================================

MateIRRuntime::MateIRRuntime(const std::string& name,
                               const Module& mod,
                               const MateIR& mate_ir,
                               const MateIRRuntimeMetadata& runtime_metadata)
    : instance_name_(name), module_def_(mod), ir_(mate_ir), metadata_(runtime_metadata)
{
    buildTopInputDomainMaps();
    buildFlopMaps();
    buildTopology();
    initConsts();
}

void MateIRRuntime::initialize(FlopsInitial mode, std::mt19937_64& rng) {
    initFlops(mode, rng);
    initXs(rng);
}

void MateIRRuntime::initializeInputsAndEvaluate(
    std::span<const RuntimeInputUpdate> async_inputs,
    std::span<const RuntimeInputUpdate> sync_inputs) {
    for (const auto& update : async_inputs) {
        initializeAsyncInput(update.input, update.value);
    }
    for (const auto& update : sync_inputs) {
        setSyncInput(update.input, update.value);
    }
    for (ResetId reset_id : activeResetDomains()) {
        applyResetDomain(reset_id);
    }
    updateOutputs();
}

void MateIRRuntime::applyClockEdge(
    ClockId clock,
    edge_t edge,
    std::span<const RuntimeInputUpdate> sync_inputs_before,
    std::span<const RuntimeInputUpdate> sync_inputs_after) {
    validateClockEdge(clock, edge);
    const RuntimeInputId source = clockSourceInput(clock);
    const auto& input_metadata = metadata_.input_leaves.at(source.value);
    setAsyncInputValue(input_metadata.leaf_name, sourceValueForEdge(source, edge));

    const bool active_edge = edge == ir_.clocks.at(clock.value).edge;
    if (!active_edge && (!sync_inputs_before.empty() || !sync_inputs_after.empty())) {
        throw CompilerError(std::format(
            "Simulator runtime: inactive {} on clock domain '{}' cannot advance sync inputs",
            edgeName(edge), ir_.clocks.at(clock.value).display_name));
    }

    for (const auto& update : sync_inputs_before) {
        validateSyncUpdateForClock(clock, update);
        setSyncInput(update.input, update.value);
    }
    for (const auto& update : sync_inputs_after) {
        validateSyncUpdateForClock(clock, update);
    }

    if (active_edge) {
        evaluateCombinational();
        applyClockDomain(clock);
        for (const auto& update : sync_inputs_after) {
            setSyncInput(update.input, update.value);
        }
    }
    updateOutputs();
}

void MateIRRuntime::applyResetEdge(ResetId reset, edge_t edge) {
    validateResetEdge(reset, edge);
    const RuntimeInputId source = resetSourceInput(reset);
    const auto& input_metadata = metadata_.input_leaves.at(source.value);
    setAsyncInputValue(input_metadata.leaf_name, sourceValueForEdge(source, edge));

    if (edge == ir_.resets.at(reset.value).active_edge) {
        applyResetDomain(reset);
    }
    updateOutputs();
}

void MateIRRuntime::applyAsyncSignalEdge(RuntimeInputId input, const SimValue& value) {
    if (input.value >= metadata_.input_leaves.size()) {
        throw CompilerError(std::format(
            "Simulator runtime: invalid input handle {}", input.value));
    }
    const auto& input_metadata = metadata_.input_leaves.at(input.value);
    if (input_metadata.kind != RuntimeInputKind::Async) {
        throw CompilerError(std::format(
            "Simulator runtime: input '{}' is not a plain async signal",
            input_metadata.leaf_name));
    }
    setAsyncInputValue(input_metadata.leaf_name, value);
    updateOutputs();
}

void MateIRRuntime::initializeAsyncInput(RuntimeInputId input, const SimValue& value) {
    if (input.value >= metadata_.input_leaves.size()) {
        throw CompilerError(std::format(
            "Simulator runtime: invalid input handle {}", input.value));
    }
    const auto& input_metadata = metadata_.input_leaves.at(input.value);
    if (input_metadata.kind == RuntimeInputKind::Sync) {
        throw CompilerError(std::format(
            "Simulator runtime: sync input '{}' cannot be initialized as async",
            input_metadata.leaf_name));
    }
    setAsyncInputValue(input_metadata.leaf_name, value);
}

void MateIRRuntime::setSyncInput(RuntimeInputId input, const SimValue& value) {
    if (input.value >= metadata_.input_leaves.size()) {
        throw CompilerError(std::format(
            "Simulator runtime: invalid input handle {}", input.value));
    }
    const auto& input_metadata = metadata_.input_leaves.at(input.value);
    if (input_metadata.kind != RuntimeInputKind::Sync) {
        throw CompilerError(std::format(
            "Simulator runtime: input '{}' is not a sync input",
            input_metadata.leaf_name));
    }
    setTopInputValue(input_metadata.leaf_name, value);
}

void MateIRRuntime::updateOutputs() {
    evaluateCombinational();
}

SimValue MateIRRuntime::getOutput(RuntimeOutputId output) const {
    if (output.value >= metadata_.output_leaves.size()) {
        throw CompilerError(std::format(
            "Simulator runtime: invalid output handle {}", output.value));
    }
    const auto& output_metadata = metadata_.output_leaves.at(output.value);
    if (!output_metadata.node) {
        throw CompilerError(std::format(
            "Simulator runtime: output '{}' has no bound DFG leaf",
            output_metadata.leaf_name));
    }
    return checkedGet(output_metadata.node);
}

SimValue MateIRRuntime::getObservable(RuntimeObservableId observable) const {
    if (observable.value >= metadata_.observables.size()) {
        throw CompilerError(std::format(
            "Simulator runtime: invalid observable handle {}", observable.value));
    }
    const auto& observable_metadata = metadata_.observables.at(observable.value);
    if (!observable_metadata.node) {
        throw CompilerError(std::format(
            "Simulator runtime: observable '{}' has no bound DFG node",
            observable_metadata.full_path));
    }
    return checkedGet(observable_metadata.node);
}

void MateIRRuntime::buildTopInputDomainMaps() {
    clock_domains_by_top_input_.clear();
    reset_domains_by_top_input_.clear();
    reset_top_input_by_id_.clear();

    clock_domains_by_top_input_ = metadata_.clock_domains_by_top_input;
    reset_domains_by_top_input_ = metadata_.reset_domains_by_top_input;
    reset_top_input_by_id_ = metadata_.reset_top_input_by_id;
}

void MateIRRuntime::setTopInputValue(const std::string& leaf_name, const SimValue& value) {
    const auto* input = metadata_.findInput(leaf_name);
    if (!input || !input->node) {
        throw CompilerError(std::format(
            "Simulator: top-level input '{}' has no bound DFG leaf", leaf_name));
    }
    values_[input->node] = value;
}

void MateIRRuntime::setAsyncInputValue(const std::string& leaf_name, const SimValue& value) {
    async_values_[leaf_name] = value;
    setTopInputValue(leaf_name, value);
}

const SimValue& MateIRRuntime::asyncInputValue(const std::string& leaf_name) const {
    auto it = async_values_.find(leaf_name);
    if (it == async_values_.end()) {
        throw CompilerError(std::format(
            "Simulator: async input '{}' has no runtime value", leaf_name));
    }
    return it->second;
}

bool MateIRRuntime::resetDomainActive(ResetId id) const {
    auto source_it = reset_top_input_by_id_.find(id);
    if (source_it == reset_top_input_by_id_.end()) {
        throw CompilerError(std::format(
            "Simulator: reset domain {} has no top-level input source", id.value));
    }
    return isActiveLevel(asyncInputValue(source_it->second), ir_.resets[id.value].active_edge);
}

std::set<ResetId> MateIRRuntime::activeResetDomains() const {
    std::set<ResetId> active;
    for (const auto& reset : ir_.resets) {
        if (resetDomainActive(reset.id)) {
            active.insert(reset.id);
        }
    }
    return active;
}

void MateIRRuntime::validateSyncUpdateForClock(ClockId clock_id,
                                               const RuntimeInputUpdate& update) const {
    if (update.input.value >= metadata_.input_leaves.size()) {
        throw CompilerError(std::format(
            "Simulator runtime: invalid sync input handle {}", update.input.value));
    }
    const auto& input_metadata = metadata_.input_leaves.at(update.input.value);
    if (input_metadata.kind != RuntimeInputKind::Sync) {
        throw CompilerError(std::format(
            "Simulator runtime: input '{}' is not a sync input",
            input_metadata.leaf_name));
    }
    if (!input_metadata.clock_domain.has_value() ||
        input_metadata.clock_domain.value() != clock_id) {
        throw CompilerError(std::format(
            "Simulator runtime: sync input '{}' does not belong to active clock domain {}",
            input_metadata.leaf_name, clock_id.value));
    }
}

RuntimeInputId MateIRRuntime::clockSourceInput(ClockId clock_id) const {
    if (clock_id.value >= ir_.clocks.size() || ir_.clocks.at(clock_id.value).id != clock_id) {
        throw CompilerError(std::format(
            "Simulator runtime: invalid clock domain {}", clock_id.value));
    }
    for (const auto& clock : metadata_.clocks) {
        if (clock.domain_id == clock_id) return clock.source_input;
    }
    throw CompilerError(std::format(
        "Simulator runtime: clock domain {} has no runtime source input", clock_id.value));
}

RuntimeInputId MateIRRuntime::resetSourceInput(ResetId reset_id) const {
    if (reset_id.value >= ir_.resets.size() || ir_.resets.at(reset_id.value).id != reset_id) {
        throw CompilerError(std::format(
            "Simulator runtime: invalid reset domain {}", reset_id.value));
    }
    for (const auto& reset : metadata_.resets) {
        if (reset.domain_id == reset_id) return reset.source_input;
    }
    throw CompilerError(std::format(
        "Simulator runtime: reset domain {} has no runtime source input", reset_id.value));
}

void MateIRRuntime::validateClockEdge(ClockId clock_id, edge_t edge) const {
    const RuntimeInputId source = clockSourceInput(clock_id);
    const auto& input_metadata = metadata_.input_leaves.at(source.value);
    if (input_metadata.kind != RuntimeInputKind::Clock) {
        throw CompilerError(std::format(
            "Simulator runtime: clock domain '{}' source '{}' is not a clock input",
            ir_.clocks.at(clock_id.value).display_name, input_metadata.leaf_name));
    }
    (void)edge;
}

void MateIRRuntime::validateResetEdge(ResetId reset_id, edge_t edge) const {
    const RuntimeInputId source = resetSourceInput(reset_id);
    const auto& input_metadata = metadata_.input_leaves.at(source.value);
    if (input_metadata.kind != RuntimeInputKind::Reset) {
        throw CompilerError(std::format(
            "Simulator runtime: reset domain '{}' source '{}' is not a reset input",
            ir_.resets.at(reset_id.value).display_name, input_metadata.leaf_name));
    }
    (void)edge;
}

SimValue MateIRRuntime::sourceValueForEdge(RuntimeInputId input, edge_t edge) const {
    if (input.value >= metadata_.input_leaves.size()) {
        throw CompilerError(std::format(
            "Simulator runtime: invalid input handle {}", input.value));
    }
    const auto& input_metadata = metadata_.input_leaves.at(input.value);
    return SimValue::fromU64(edge == POSEDGE ? 1 : 0,
                             input_metadata.type.width,
                             input_metadata.type.isSigned());
}

void MateIRRuntime::applyResetDomain(ResetId reset_id) {
    auto flop_it = flops_by_reset_.find(reset_id);
    if (flop_it == flops_by_reset_.end()) return;
    for (const auto& collected : flop_it->second) {
        const auto* flop = collected.flop;
        if (flop->reset_value.has_value()) {
            assignFlopResetLeaves(*flop, flop->reset_value.value());
        }
    }
}

void MateIRRuntime::applyClockDomain(ClockId clock_id) {
    auto flop_it = flops_by_clock_.find(clock_id);
    if (flop_it == flops_by_clock_.end()) return;
    for (const auto& collected : flop_it->second) {
        const auto* flop = collected.flop;
        bool reset_active = false;
        for (ResetId reset_id : collected.reset_domains.ids) {
            if (resetDomainActive(reset_id)) {
                reset_active = true;
                break;
            }
        }
        if (reset_active) continue;
        copyFlopDToQLeaves(*flop);
    }
}

void MateIRRuntime::assignFlopResetLeaves(const FlopInfo& flop, int64_t reset_value) {
    for (auto* q_leaf : flopQLeaves(flop)) {
        if (!q_leaf) continue;
        const Type& type = q_leaf->type.value_or(flop.type);
        values_[q_leaf] = simValueFromType(reset_value, type);
    }
}

void MateIRRuntime::copyFlopDToQLeaves(const FlopInfo& flop) {
    const auto& q_leaves = flopQLeaves(flop);
    const auto& d_leaves = flopDLeaves(flop);
    if (q_leaves.size() != d_leaves.size()) {
        throw CompilerError(std::format(
            "Simulator: flop '{}' has mismatched d/q leaf counts ({} vs {})",
            flop.name, d_leaves.size(), q_leaves.size()));
    }
    for (size_t i = 0; i < q_leaves.size(); ++i) {
        auto* q_leaf = q_leaves[i];
        auto* d_leaf = d_leaves[i];
        if (q_leaf && d_leaf) {
            values_[q_leaf] = checkedGet(d_leaf);
        }
    }
}

// ============================================================================
// Bit mask helper
// ============================================================================

SimValue MateIRRuntime::maskToWidth(const SimValue& val, const DFGNode* node) {
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

void MateIRRuntime::buildTopology() {
    const auto& nodes = module_def_.dfg->nodes;
    node_indices_.clear();
    for (size_t i = 0; i < nodes.size(); ++i) {
        node_indices_[nodes[i].get()] = i;
    }

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

// ============================================================================
// Build flop lookup maps
// ============================================================================

void MateIRRuntime::buildFlopMaps() {
    flop_q_nodes_.clear();
    flops_by_clock_.clear();
    flops_by_reset_.clear();

    for (const auto& runtime_flop : metadata_.flops) {
        const auto* flop = runtime_flop.flop;
        if (!flop) {
            throw CompilerError(std::format(
                "Simulator: runtime flop '{}' has no source flop", runtime_flop.name));
        }
        for (const auto& leaf : runtime_flop.leaves) {
            if (leaf.q_node) flop_q_nodes_[leaf.q_node] = flop;
        }
        CollectedFlop collected{
            .flop = flop,
            .clock_domain = runtime_flop.clock_domain,
            .reset_domains = runtime_flop.reset_domains,
        };
        for (ResetId resetId : collected.reset_domains.ids) {
            flops_by_reset_[resetId].push_back(collected);
        }
        flops_by_clock_[collected.clock_domain].push_back(std::move(collected));
    }
}

// ============================================================================
// Initialize constant node values_
// ============================================================================

void MateIRRuntime::initConsts() {
    for (const auto& node : module_def_.dfg->nodes) {
        if (node->kind() == DFGOp::CONST) {
            values_[node.get()] = simValueFromInt(node->constValue(), node.get());
        }
    }
}

void MateIRRuntime::initXs(std::mt19937_64& rng) {
    for (const auto& node : module_def_.dfg->nodes) {
        if (node->kind() != DFGOp::X) continue;
        if (!node->type.has_value()) {
            throw CompilerError("Simulator: X node has no type", node.get());
        }
        values_[node.get()] = SimValue::random(node->type->width, node->type->isSigned(), rng);
    }
}

// ============================================================================
// Initialize flop values_ (recursive)
// ============================================================================

void MateIRRuntime::initFlops(FlopsInitial mode, std::mt19937_64& rng) {
    // flop_q_nodes_ already covers all flops from all submodules (built by buildFlopMaps)
    for (const auto& [qnode, flop] : flop_q_nodes_) {
        const Type& type = qnode->type.value_or(flop->type);
        int w = type.width;
        if (mode == FlopsInitial::Random) {
            values_[qnode] = SimValue::random(w, type.isSigned(), rng);
        } else if (mode == FlopsInitial::AllOnes) {
            values_[qnode] = SimValue::ones(w, type.isSigned());
        } else {
            values_[qnode] = simValueFromType(0, type);
        }
    }
}

// ============================================================================
// Node evaluation
// ============================================================================

const SimValue& MateIRRuntime::checkedGetRef(const DFGNode* node, const DFGNode* context) const {
    auto it = values_.find(node);
    if (it == values_.end())
        throw CompilerError(std::format(
            "Simulator: node {} has no computed value{}",
            node->str(),
            context && context != node
                ? std::format(" (while evaluating {})", context->str())
                : ""),
            context ? context : node);
    return it->second;
}

SimValue MateIRRuntime::checkedGet(const DFGNode* node, const DFGNode* context) const {
    return checkedGetRef(node, context);
}

const SimValue* MateIRRuntime::findNodeValue(const DFGNode* node) const {
    auto it = values_.find(node);
    if (it == values_.end()) return nullptr;
    return &it->second;
}

bool MateIRRuntime::isFlopQNode(const DFGNode* node) const {
    return flop_q_nodes_.contains(node);
}

std::optional<size_t> MateIRRuntime::nodeIndex(const DFGNode* node) const {
    auto it = node_indices_.find(node);
    if (it == node_indices_.end()) return std::nullopt;
    return it->second;
}

SimValue MateIRRuntime::evaluateNode(const DFGNode* node) {
    auto getUnaryVal = [&]() -> const SimValue& {
        return checkedGetRef(node->unaryInputs().operand.node, node);
    };
    auto emitTrace = [&](const std::vector<std::pair<std::string, SimValue>>& inputs,
                         const SimValue& result,
                         const std::string& decisions_json = "") {
        if (trace_sink) {
            trace_sink(node, inputs, result, decisions_json);
        }
    };

    switch (node->kind()) {
        case DFGOp::INPUT:
        case DFGOp::CONST:
        case DFGOp::X:
            return checkedGetRef(node);

        case DFGOp::SIGNAL:
        case DFGOp::OUTPUT:
            if (auto driver = node->driver()) {
                SimValue result = checkedGetRef(driver->node, node);
                emitTrace({{"driver", result}}, result);
                return result;
            }
            return checkedGetRef(node);

        case DFGOp::ADD: {
            auto inputs = node->binaryInputs();
            const SimValue& lhsRaw = checkedGetRef(inputs.lhs.node, node);
            const SimValue& rhsRaw = checkedGetRef(inputs.rhs.node, node);
            SimValue lhs = widenForArithmetic(lhsRaw, inputs.lhs.node, inputs.rhs.node, node);
            SimValue rhs = widenForArithmetic(rhsRaw, inputs.rhs.node, inputs.lhs.node, node);
            SimValue result = maskToWidth(lhs.add(rhs), node);
            emitTrace({{"lhs", lhsRaw}, {"rhs", rhsRaw}}, result,
                      std::format(
                          "\"signed\":{},\"lhs_extended_width\":{},\"rhs_extended_width\":{}",
                          (nodeSigned(inputs.lhs.node) && nodeSigned(inputs.rhs.node)) ? "true" : "false",
                          lhs.width(), rhs.width()));
            return result;
        }
        case DFGOp::SUB: {
            auto inputs = node->binaryInputs();
            const SimValue& lhsRaw = checkedGetRef(inputs.lhs.node, node);
            const SimValue& rhsRaw = checkedGetRef(inputs.rhs.node, node);
            SimValue lhs = widenForArithmetic(lhsRaw, inputs.lhs.node, inputs.rhs.node, node);
            SimValue rhs = widenForArithmetic(rhsRaw, inputs.rhs.node, inputs.lhs.node, node);
            SimValue result = maskToWidth(lhs.sub(rhs), node);
            emitTrace({{"lhs", lhsRaw}, {"rhs", rhsRaw}}, result,
                      std::format(
                          "\"signed\":{},\"lhs_extended_width\":{},\"rhs_extended_width\":{}",
                          (nodeSigned(inputs.lhs.node) && nodeSigned(inputs.rhs.node)) ? "true" : "false",
                          lhs.width(), rhs.width()));
            return result;
        }
        case DFGOp::MUL: {
            auto inputs = node->binaryInputs();
            const SimValue& lhsRaw = checkedGetRef(inputs.lhs.node, node);
            const SimValue& rhsRaw = checkedGetRef(inputs.rhs.node, node);
            SimValue lhs = widenForArithmetic(lhsRaw, inputs.lhs.node, inputs.rhs.node, node);
            SimValue rhs = widenForArithmetic(rhsRaw, inputs.rhs.node, inputs.lhs.node, node);
            SimValue result = maskToWidth(lhs.mul(rhs), node);
            emitTrace({{"lhs", lhsRaw}, {"rhs", rhsRaw}}, result,
                      std::format(
                          "\"signed\":{},\"lhs_extended_width\":{},\"rhs_extended_width\":{}",
                          (nodeSigned(inputs.lhs.node) && nodeSigned(inputs.rhs.node)) ? "true" : "false",
                          lhs.width(), rhs.width()));
            return result;
        }

        case DFGOp::EQ: {
            auto inputs = node->binaryInputs();
            const SimValue& lhs = checkedGetRef(inputs.lhs.node, node);
            const SimValue& rhs = checkedGetRef(inputs.rhs.node, node);
            SimValue result = boolValue(lhs.eq(rhs));
            emitTrace({{"lhs", lhs}, {"rhs", rhs}}, result);
            return result;
        }
        case DFGOp::LT: {
            auto inputs = node->binaryInputs();
            const SimValue& lhs = checkedGetRef(inputs.lhs.node, node);
            const SimValue& rhs = checkedGetRef(inputs.rhs.node, node);
            const bool is_signed = useSignedCompare(inputs.lhs.node, inputs.rhs.node);
            SimValue result = boolValue(is_signed
                ? lhs.signedLt(rhs)
                : lhs.unsignedLt(rhs));
            emitTrace({{"lhs", lhs}, {"rhs", rhs}}, result,
                      std::format("\"signed\":{}", is_signed ? "true" : "false"));
            return result;
        }
        case DFGOp::LE: {
            auto inputs = node->binaryInputs();
            const SimValue& lhs = checkedGetRef(inputs.lhs.node, node);
            const SimValue& rhs = checkedGetRef(inputs.rhs.node, node);
            bool is_signed = useSignedCompare(inputs.lhs.node, inputs.rhs.node);
            bool lt = is_signed
                ? lhs.signedLt(rhs)
                : lhs.unsignedLt(rhs);
            SimValue result = boolValue(lt || lhs.eq(rhs));
            emitTrace({{"lhs", lhs}, {"rhs", rhs}}, result,
                      std::format("\"signed\":{}", is_signed ? "true" : "false"));
            return result;
        }
        case DFGOp::GT: {
            auto inputs = node->binaryInputs();
            const SimValue& lhs = checkedGetRef(inputs.lhs.node, node);
            const SimValue& rhs = checkedGetRef(inputs.rhs.node, node);
            const bool is_signed = useSignedCompare(inputs.lhs.node, inputs.rhs.node);
            SimValue result = boolValue(is_signed
                ? rhs.signedLt(lhs)
                : rhs.unsignedLt(lhs));
            emitTrace({{"lhs", lhs}, {"rhs", rhs}}, result,
                      std::format("\"signed\":{}", is_signed ? "true" : "false"));
            return result;
        }
        case DFGOp::GE: {
            auto inputs = node->binaryInputs();
            const SimValue& lhs = checkedGetRef(inputs.lhs.node, node);
            const SimValue& rhs = checkedGetRef(inputs.rhs.node, node);
            bool is_signed = useSignedCompare(inputs.lhs.node, inputs.rhs.node);
            bool lt = is_signed
                ? lhs.signedLt(rhs)
                : lhs.unsignedLt(rhs);
            SimValue result = boolValue(!lt);
            emitTrace({{"lhs", lhs}, {"rhs", rhs}}, result,
                      std::format("\"signed\":{}", is_signed ? "true" : "false"));
            return result;
        }

        case DFGOp::SHL: {
            auto inputs = node->binaryInputs();
            const SimValue& lhs = checkedGetRef(inputs.lhs.node, node);
            const SimValue& rhs = checkedGetRef(inputs.rhs.node, node);
            SimValue result = lhs.shl(rhs.lowU64());
            emitTrace({{"lhs", lhs}, {"rhs", rhs}}, result);
            return result;
        }
        case DFGOp::ASR: {
            auto inputs = node->binaryInputs();
            const SimValue& lhs = checkedGetRef(inputs.lhs.node, node);
            const SimValue& rhs = checkedGetRef(inputs.rhs.node, node);
            SimValue result = lhs.shr(rhs.lowU64(), true);
            emitTrace({{"lhs", lhs}, {"rhs", rhs}}, result);
            return result;
        }

        case DFGOp::MUX:
        {
            int64_t selectorValue = static_cast<int64_t>(checkedGetRef(node->muxSelector().node, node).lowU64());
            int armIndex = node->muxArmIndexForValue(selectorValue);
            if (armIndex < 0) {
                throw CompilerError(
                    std::format("Simulator: MUX {} has no arm for selector value {}",
                        node->str(), selectorValue),
                    node);
            }
            SimValue result = checkedGetRef(node->muxArmData(static_cast<size_t>(armIndex)).node, node);
            emitTrace({{"selector", checkedGetRef(node->muxSelector().node, node)}}, result,
                      std::format(
                          "\"selected_arm_index\":{},\"selected_arm_value\":{},\"selected_arm_debug_id\":{}",
                          armIndex, node->muxArmValue(static_cast<size_t>(armIndex)),
                          node->muxArmData(static_cast<size_t>(armIndex)).node->debug_id));
            return result;
        }

        case DFGOp::UNARY_NEGATE:  {
            SimValue operand = getUnaryVal();
            SimValue result = operand.negated();
            emitTrace({{"operand", operand}}, result);
            return result;
        }
        case DFGOp::BITWISE_NOT:   {
            SimValue operand = getUnaryVal();
            SimValue result = operand.bitwiseNot();
            emitTrace({{"operand", operand}}, result);
            return result;
        }
        case DFGOp::BITWISE_AND: {
            auto inputs = node->binaryInputs();
            const SimValue& lhs = checkedGetRef(inputs.lhs.node, node);
            const SimValue& rhs = checkedGetRef(inputs.rhs.node, node);
            SimValue result = lhs.bitwiseAnd(rhs);
            emitTrace({{"lhs", lhs}, {"rhs", rhs}}, result);
            return result;
        }
        case DFGOp::BITWISE_OR: {
            auto inputs = node->binaryInputs();
            const SimValue& lhs = checkedGetRef(inputs.lhs.node, node);
            const SimValue& rhs = checkedGetRef(inputs.rhs.node, node);
            SimValue result = lhs.bitwiseOr(rhs);
            emitTrace({{"lhs", lhs}, {"rhs", rhs}}, result);
            return result;
        }
        case DFGOp::BITWISE_XOR: {
            auto inputs = node->binaryInputs();
            const SimValue& lhs = checkedGetRef(inputs.lhs.node, node);
            const SimValue& rhs = checkedGetRef(inputs.rhs.node, node);
            SimValue result = lhs.bitwiseXor(rhs);
            emitTrace({{"lhs", lhs}, {"rhs", rhs}}, result);
            return result;
        }
        case DFGOp::BITWISE_XNOR: {
            auto inputs = node->binaryInputs();
            const SimValue& lhs = checkedGetRef(inputs.lhs.node, node);
            const SimValue& rhs = checkedGetRef(inputs.rhs.node, node);
            SimValue result = lhs.bitwiseXnor(rhs);
            emitTrace({{"lhs", lhs}, {"rhs", rhs}}, result);
            return result;
        }

        case DFGOp::REDUCTION_AND:
        {
            SimValue operand = getUnaryVal();
            SimValue result = boolValue(operand.reductionAnd());
            emitTrace({{"operand", operand}}, result);
            return result;
        }
        case DFGOp::REDUCTION_NAND:
        {
            SimValue operand = getUnaryVal();
            SimValue result = boolValue(!operand.reductionAnd());
            emitTrace({{"operand", operand}}, result);
            return result;
        }
        case DFGOp::REDUCTION_OR:
        {
            SimValue operand = getUnaryVal();
            SimValue result = boolValue(operand.reductionOr());
            emitTrace({{"operand", operand}}, result);
            return result;
        }
        case DFGOp::REDUCTION_NOR:
        {
            SimValue operand = getUnaryVal();
            SimValue result = boolValue(!operand.reductionOr());
            emitTrace({{"operand", operand}}, result);
            return result;
        }
        case DFGOp::REDUCTION_XOR:
        {
            SimValue operand = getUnaryVal();
            SimValue result = boolValue(operand.reductionXor());
            emitTrace({{"operand", operand}}, result);
            return result;
        }
        case DFGOp::REDUCTION_XNOR:
        {
            SimValue operand = getUnaryVal();
            SimValue result = boolValue(!operand.reductionXor());
            emitTrace({{"operand", operand}}, result);
            return result;
        }

        case DFGOp::SLICE: {
            // SLICE has CONST high/low. Dynamic indexing is lowered to MUX during elaboration.
            // Unpacked array access is lowered to scalar leaves or MUX-over-leaves;
            // aggregate SIGNAL nodes no longer appear in the live DFG.
            auto slice = node->sliceInputs();
            const DFGNode* source_node = slice.source.node;

            int64_t high = static_cast<int64_t>(checkedGetRef(slice.high.node, node).lowU64());
            int64_t low = static_cast<int64_t>(checkedGetRef(slice.low.node, node).lowU64());
            auto resolved = resolveSliceRange(*source_node, high, low);
            SimValue result = checkedGetRef(slice.source.node, node).slice(
                static_cast<int>(resolved.internal_high), static_cast<int>(resolved.internal_low));
            emitTrace(
                {{"source", checkedGetRef(slice.source.node, node)},
                 {"high", checkedGetRef(slice.high.node, node)},
                 {"low", checkedGetRef(slice.low.node, node)}},
                result,
                std::format(
                    "\"internal_low\":{},\"internal_high\":{},\"width\":{}",
                    resolved.internal_low, resolved.internal_high, resolved.width));
            return result;
        }

        case DFGOp::CONCAT: {
            std::vector<SimValue> parts;
            parts.reserve(node->concatParts().size());
            std::vector<std::pair<std::string, SimValue>> inputs;
            inputs.reserve(node->concatParts().size());
            for (const auto& part : node->concatParts()) {
                SimValue value = checkedGetRef(part.node, node);
                parts.push_back(value);
                inputs.push_back({dfgInputRole(*node, inputs.size()), value});
            }
            SimValue result = SimValue::concat(parts);
            emitTrace(inputs, result);
            return result;
        }
    }

    throw CompilerError(std::format("Simulator: unhandled op {}", to_string(node->kind())), node);
}

// ============================================================================
// Combinational evaluation — single topo-ordered pass (flat DAG, no cycles)
// ============================================================================

void MateIRRuntime::evaluateCombinational() {
    for (const DFGNode* node : topo_order_) {
        if (node->kind() == DFGOp::INPUT || node->kind() == DFGOp::CONST || node->kind() == DFGOp::X) continue;
        if (flop_q_nodes_.count(node)) continue;
        SimValue val = maskToWidth(evaluateNode(node), node);
        values_[node] = val;
    }
}

// ============================================================================

} // namespace mate
