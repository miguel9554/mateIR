#include "passes/flop_resolve.h"
#include "ir/dfg.h"
#include "util/source_loc.h"

#include <algorithm>
#include <format>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace custom_hdl {

namespace {

// Generate all index suffixes for multi-dimensional arrays
// For [0:1], returns ["[0]", "[1]"]
// For [0:1][0:1], returns ["[0][0]", "[[0][1]", "[1][0]", "[1][1]"]
std::vector<std::string> generateIndexSuffixes(const std::vector<ResolvedDimension>& dimensions) {
    if (dimensions.empty()) {
        return {""};
    }

    std::vector<std::string> result = {""};
    for (const auto& dim : dimensions) {
        std::vector<std::string> newResult;
        int step = (dim.left <= dim.right) ? 1 : -1;
        for (int i = dim.left; step > 0 ? i <= dim.right : i >= dim.right; i += step) {
            for (const auto& prefix : result) {
                newResult.push_back(prefix + "[" + std::to_string(i) + "]");
            }
        }
        result = std::move(newResult);
    }
    return result;
}

// Return all leaf element names for a signal (expanding dimensions)
std::vector<std::string> allElements(const ResolvedSignal& signal) {
    std::vector<std::string> current = {signal.name};

    for (const auto& dimension : signal.type.unpacked_dims) {
        std::vector<std::string> next;
        int start = std::min(dimension.left, dimension.right);
        int end = std::max(dimension.left, dimension.right);
        for (const auto& prefix : current) {
            for (int i = start; i <= end; i++) {
                next.push_back(prefix + "[" + std::to_string(i) + "]");
            }
        }
        current = std::move(next);
    }

    return current;
}

// Prefix a module-local name with the instance path for DFG node lookups.
// Empty instance_path means top module — no prefix needed.
std::string inDFG(const std::string& instance_path, const std::string& name) {
    return instance_path.empty() ? name : instance_path + "." + name;
}

DFGNode* nodeForTrigger(const ResolvedModule& resolved, const asyncTrigger_t& trigger) {
    auto it = resolved.inputs.find(trigger.name);
    return it != resolved.inputs.end() ? it->second.dfg_node : nullptr;
}

bool isFlopQValue(const DFGNode* node, const std::string& flop_name) {
    if (node->op == DFGOp::INPUT) {
        return node->name == flop_name + ".q";
    }
    if (node->op != DFGOp::INDEX) {
        return false;
    }
    auto* source = node->in[0].node;
    return source->op == DFGOp::INPUT && source->name == flop_name + ".q";
}

int64_t maskForWidth(int width) {
    if (width <= 0) return 0;
    if (width >= 64) return ~0LL;
    return (1LL << width) - 1;
}

std::optional<int64_t> constantValueOfNode(const DFGNode* node) {
    if (node->op == DFGOp::CONST) {
        return std::get<int64_t>(node->data);
    }
    if (node->op == DFGOp::INDEX) {
        auto src = constantValueOfNode(node->in[0].node);
        if (!src) return std::nullopt;
        auto hi = constantValueOfNode(node->in[1].node);
        auto lo = constantValueOfNode(node->in[2].node);
        if (!hi || !lo || *hi < *lo) return std::nullopt;
        int width = static_cast<int>(*hi - *lo + 1);
        return (*src >> *lo) & maskForWidth(width);
    }
    if (node->op == DFGOp::CONCAT) {
        int64_t value = 0;
        for (const auto& input : node->in) {
            auto part = constantValueOfNode(input.node);
            if (!part || !input.node->type) return std::nullopt;
            int width = input.node->type->width;
            if (width <= 0 || width > 63) return std::nullopt;
            value = (value << width) | (*part & maskForWidth(width));
        }
        return value;
    }
    return std::nullopt;
}

bool extract_reset_from_concat(
    DFG& graph,
    const DFGNode* dNodeDriver,
    const std::vector<asyncTrigger_t>& triggers,
    const std::string& flop_name,
    const ResolvedModule& resolved,
    asyncTrigger_t& reset,
    asyncTrigger_t& clock,
    int& reset_value,
    DFGNode*& functionalLogic)
{
    if (dNodeDriver->in.empty()) {
        throw CompilerError(
            std::format("flop '{}' reset CONCAT has no inputs", flop_name),
            dNodeDriver->loc);
    }

    const DFGNode* firstMux = dNodeDriver->in[0].node;
    if (firstMux->op != DFGOp::MUX) {
        throw CompilerError(
            std::format("flop '{}' reset CONCAT input is not a MUX (op={})",
                flop_name, to_string(firstMux->op)),
            firstMux->loc);
    }
    if (!firstMux->isBinaryMux()) {
        throw CompilerError(
            std::format("flop '{}' reset CONCAT input MUX is not binary", flop_name),
            firstMux->loc);
    }

    auto* mux_sel = firstMux->muxSelector().node;
    if (nodeForTrigger(resolved, triggers[0]) == mux_sel) {
        reset = triggers[0];
        clock = triggers[1];
    } else if (nodeForTrigger(resolved, triggers[1]) == mux_sel) {
        reset = triggers[1];
        clock = triggers[0];
    } else {
        throw CompilerError(std::format(
            "flop '{}' has 2 triggers but CONCAT/MUX selector '{}' matches neither trigger ('{}', '{}')",
            flop_name, mux_sel->name, triggers[0].name, triggers[1].name), dNodeDriver->loc);
    }

    std::vector<DFGNode*> functionalParts;
    functionalParts.reserve(dNodeDriver->in.size());

    bool resetIsConst = true;
    bool resetIsQ = true;
    int64_t assembledReset = 0;

    for (const auto& input : dNodeDriver->in) {
        auto* mux = input.node;
        if (mux->op != DFGOp::MUX) {
            throw CompilerError(
                std::format("flop '{}' reset CONCAT contains non-MUX input (op={})",
                    flop_name, to_string(mux->op)),
                mux->loc);
        }
        if (!mux->isBinaryMux()) {
            throw CompilerError(
                std::format("flop '{}' reset CONCAT contains non-binary MUX", flop_name),
                mux->loc);
        }
        if (mux->muxSelector().node != mux_sel) {
            throw CompilerError(
                std::format("flop '{}' reset CONCAT mixes different selectors", flop_name),
                mux->loc);
        }

        auto* resetBranch = mux->muxDataForValue(reset.edge == edge_t::POSEDGE ? 1 : 0);
        auto* functionalBranch = mux->muxDataForValue(reset.edge == edge_t::POSEDGE ? 0 : 1);
        functionalParts.push_back(functionalBranch);

        int partWidth = mux->type ? mux->type->width : (resetBranch->type ? resetBranch->type->width : 0);
        if (partWidth <= 0 || partWidth > 63) {
            throw CompilerError(
                std::format("flop '{}' reset CONCAT contains unsupported slice width {}", flop_name, partWidth),
                mux->loc);
        }

        if (auto constValue = constantValueOfNode(resetBranch)) {
            assembledReset = (assembledReset << partWidth) |
                (*constValue & maskForWidth(partWidth));
            resetIsQ = false;
        } else if (isFlopQValue(resetBranch, flop_name)) {
            resetIsConst = false;
        } else {
            throw CompilerError(
                "Unsupported reset CONCAT branch: " + resetBranch->str(),
                resetBranch->loc ? resetBranch->loc : dNodeDriver->loc);
        }

        if (!resetIsConst && !resetIsQ) {
            throw CompilerError(
                std::format("flop '{}' reset CONCAT mixes reset constants and retained .q slices", flop_name),
                mux->loc);
        }
    }

    if (resetIsConst) {
        reset_value = static_cast<int>(assembledReset);
    } else if (!resetIsQ) {
        throw CompilerError(
            std::format("flop '{}' reset CONCAT is not a pure reset constant nor retained .q", flop_name),
            dNodeDriver->loc);
    }

    functionalLogic = functionalParts.size() == 1 ? functionalParts[0] : graph.concat(functionalParts);
    functionalLogic->type = dNodeDriver->type;
    functionalLogic->loc = dNodeDriver->loc;
    return resetIsConst;
}

bool extract_reset(
    DFG& graph,
    const DFGNode* dNodeDriver,
    const std::vector<asyncTrigger_t>& triggers,
    const std::string& flop_name,
    const ResolvedModule& resolved,
    asyncTrigger_t& reset,
    asyncTrigger_t& clock,
    int& reset_value,
    DFGNode*& functionalLogic)
{
    if (dNodeDriver->op == DFGOp::CONCAT) {
        return extract_reset_from_concat(
            graph, dNodeDriver, triggers, flop_name, resolved,
            reset, clock, reset_value, functionalLogic);
    }

    // With 2 triggers the driver must be a reset MUX, or a CONCAT of reset MUX slices.
    if (dNodeDriver->op != DFGOp::MUX) {
        throw CompilerError(std::format(
            "flop '{}' has 2 triggers but its .d driver is neither MUX nor CONCAT-of-MUX (op={})",
            flop_name, to_string(dNodeDriver->op)), dNodeDriver->loc);
    }
    if (!dNodeDriver->isBinaryMux()) {
        throw CompilerError(
            std::format("flop '{}' has 2 triggers but its .d driver MUX is not binary", flop_name),
            dNodeDriver->loc);
    }

    auto* mux_sel = dNodeDriver->muxSelector().node;
    auto* mux_true = dNodeDriver->muxDataForValue(1);
    auto* mux_else = dNodeDriver->muxDataForValue(0);
    DFGNode* expectedResetAssign;

    // Match the MUX selector to one of the triggers (the reset). Compare by node identity
    // rather than name, because after DFG inlining the trigger's INPUT node is replaced
    // by the parent's signal node (with a different name), but resolved.inputs[name].dfg_node
    // is updated to track that replacement.
    if (nodeForTrigger(resolved, triggers[0]) == mux_sel) {
        reset = triggers[0];
        clock = triggers[1];
    } else if (nodeForTrigger(resolved, triggers[1]) == mux_sel) {
        reset = triggers[1];
        clock = triggers[0];
    } else {
        throw CompilerError(std::format(
            "flop '{}' has 2 triggers but MUX selector '{}' matches neither trigger ('{}', '{}')",
            flop_name, mux_sel->name, triggers[0].name, triggers[1].name), dNodeDriver->loc);
    }

    // Assign the expected reset and functional branches
    expectedResetAssign = reset.edge == edge_t::POSEDGE ? mux_true : mux_else;
    functionalLogic = reset.edge == edge_t::POSEDGE ? mux_else : mux_true;

    // Check the reset assignment is either a CONSTANT (has reset) or its .q value (NO reset)
    bool has_reset;
    if (auto constValue = constantValueOfNode(expectedResetAssign)) {
        reset_value = static_cast<int>(*constValue);
        has_reset = true;
    } else if (isFlopQValue(expectedResetAssign, flop_name)) {
        has_reset = false;
    } else {
        throw CompilerError("Unsupported MUX TRUE branch for reset: " + expectedResetAssign->str(), dNodeDriver->loc);
    }

    return has_reset;
}

FlopInfo extractFlopClockAndReset(
    DFG& graph,
    ResolvedModule& resolved,
    const std::string& flop_name,
    const std::string& instance_path,
    const FlopInfo& flopIn,
    DFGNode*& functionalLogic)
{
    auto flop = flopIn;
    DFGNode* dNode = flopIn.d_node ? flopIn.d_node : graph.getOutputNode(instance_path, flop_name + ".d");
    auto* dNodeDriver = dNode->in[0].node;
    const auto& triggers = resolved.flopsTriggers.at(flopIn.name);

    if (dNode->in.size() != 1) {
        throw CompilerError(std::format(
            "Flop must have single driver: {} has {}", instance_path.empty() ? flop_name : instance_path + "." + flop_name + ".d", dNode->in.size()), dNode->loc);
    }

    asyncTrigger_t clock;
    asyncTrigger_t reset;
    bool has_reset;
    int reset_value;

    if (triggers.size() == 1) {
        clock = triggers[0];
        functionalLogic = dNodeDriver;
        has_reset = false;
    } else if (triggers.size() == 2) {
        has_reset = extract_reset(
            graph, dNodeDriver, triggers, flop_name, resolved,
            reset, clock, reset_value, functionalLogic);
    } else {
        throw CompilerError(std::format(
            "Trigger size not supported: {}", triggers.size()), dNode->loc);
    }

    flop.clock = clock;
    if (has_reset) {
        flop.reset = reset;
        flop.reset_value = reset_value;
    }
    flop.name = flop_name;
    flop.type.name = flop_name;
    flop.type.type.unpacked_dims = {};
    return flop;
}

void check_logic_no_clock_reset(
    DFGNode* root,
    const std::string& rootName,
    const std::string& moduleName,
    const std::vector<std::string>& clocks,
    const std::vector<std::string>& resets)
{
    std::set<DFGNode*> visited;
    std::vector<DFGNode*> to_visit = {root};

    while (!to_visit.empty()) {
        DFGNode* current = to_visit.back();
        to_visit.pop_back();

        if (visited.contains(current)) {
            continue;
        }
        visited.insert(current);

        // Don't traverse into submodule inputs — clocks/resets driving a MODULE node
        // are valid (used inside the submodule for clocking, not as combinational inputs).
        if (current->op == DFGOp::MODULE) continue;

        if (current->op == DFGOp::INPUT) {
            auto loc = current->loc ? current->loc : root->loc;
            for (const auto& clk : clocks) {
                if (current->name == clk) {
                    throw CompilerError(std::format(
                        "module '{}': logic for '{}' uses clock signal '{}'",
                        moduleName, rootName, clk), loc);
                }
            }
            for (const auto& rst : resets) {
                if (current->name == rst) {
                    throw CompilerError(std::format(
                        "module '{}': logic for '{}' uses reset signal '{}'",
                        moduleName, rootName, rst), loc);
                }
            }
        }

        for (const auto& inp : current->in) {
            to_visit.push_back(inp.node);
        }
    }
}

} // anonymous namespace

// Forward declarations
static void resolveFlopsForModule(ResolvedModule& resolved, DFG& graph, const std::string& instance_path);
static void resolveFlopsRecursive(ResolvedModule& module, DFG& topDFG, const std::string& instance_path);

void resolveFlops(ResolvedModule& module) {
    if (!module.dfg) return;
    resolveFlopsRecursive(module, *module.dfg, "");
}

static void resolveFlopsRecursive(ResolvedModule& module, DFG& topDFG, const std::string& instance_path) {
    // Bottom-up: resolve submodules first so their clock/reset types are tagged first
    for (auto& sub : module.hierarchyInstantiation)
        resolveFlopsRecursive(sub, topDFG, inDFG(instance_path, sub.instance_name));
    resolveFlopsForModule(module, topDFG, instance_path);
}

static void resolveFlopsForModule(ResolvedModule& resolved, DFG& graph, const std::string& instance_path) {
    if (resolved.flops.empty()) {
        return;
    }

    // Connect flop output port nodes to their .q INPUT nodes.
    // Uses dfg_node pointer directly since after DFG inlining, submodule output ports
    // may not be in the flat top DFG's outputs map (only .d nodes are adopted).
    for (auto& [outName, output] : resolved.outputs) {
        if (!resolved.flopsTriggers.contains(outName)) continue;
        if (output.type.unpacked_dims.empty()) {
            DFGNode* qNode = graph.getInputNode(instance_path, outName + ".q");
            if (qNode && output.dfg_node) {
                output.dfg_node->in = {{qNode, 0}};
            }
        } else {
            for (const auto& suffix : generateIndexSuffixes(output.type.unpacked_dims)) {
                DFGNode* qNode = graph.getInputNode(instance_path, outName + suffix + ".q");
                // Individual element output nodes may also be in dfg outputs map
                // or just exist as nodes in the flat DFG; use the map if available.
                DFGNode* outNode = graph.getOutputNode(instance_path, outName + suffix);
                if (!outNode) {
                    // For submodule outputs not in flat DFG outputs map,
                    // there's no per-element dfg_node stored; skip.
                    continue;
                }
                if (qNode) {
                    outNode->in = {{qNode, 0}};
                }
            }
        }
    }

    // Extract clock/reset info and validate functional logic
    std::vector<FlopInfo> resolved_flops;
    for (const auto& flop : resolved.flops) {
        for (const auto& name : allElements(flop.type)) {
            DFGNode* functional_logic;
            resolved_flops.push_back(
                extractFlopClockAndReset(graph, resolved, name, instance_path, flop, functional_logic));
            // Set per-element d_node/q_node (the copy from flopIn may be null for vectorized)
            {
                resolved_flops.back().d_node = graph.getOutputNode(instance_path, name + ".d");
                resolved_flops.back().q_node = graph.getInputNode(instance_path, name + ".q");
            }
            DFGNode* output = resolved_flops.back().d_node;
            const asyncTrigger_t clock = resolved_flops.back().clock;
            const std::optional<asyncTrigger_t> reset = resolved_flops.back().reset;

            // Helper to find exactly one signal
            auto find_unique_input = [&](const std::string& sig_name, const char* role) -> ResolvedSignal& {
                auto it = resolved.inputs.find(sig_name);
                if (it == resolved.inputs.end())
                    throw CompilerError(std::format(
                        "flop_resolve: flop '{}' in module '{}' references {} signal '{}' "
                        "which is not an input port",
                        name, resolved.name, role, sig_name));
                return it->second;
            };

            // Validate clock/reset are input ports; do NOT override sync_kind here.
            // sync_kind was set by io_domains_set from the domains file and must not
            // be overwritten — domains_propagate_and_check will verify consistency.
            find_unique_input(clock.name, "clock");  // validate it's an input port
            if (reset) {
                find_unique_input(reset->name, "reset");  // validate it's an input port
            }

            // Connect functional logic to the flop's .d signal
            // If there was reset, this removes the reset MUX
            output->in = {functional_logic};
        }
    }
    resolved.flops = resolved_flops;

    // Set clock_domain/clock_edge on each flop's type signal
    for (auto& flop : resolved.flops) {
        auto it = resolved.inputs.find(flop.clock.name);
        if (it != resolved.inputs.end()) {
            flop.type.clock_domain = &it->second;
            flop.type.clock_edge = flop.clock.edge;
        }
    }

    // Build clock/reset name lists from inputs that were tagged
    std::vector<std::string> clocks;
    std::vector<std::string> resets;
    for (const auto& [name, input] : resolved.inputs) {
        if (input.sync_kind == SyncKind::Clock) {
            clocks.push_back(name);
        } else if (input.sync_kind == SyncKind::Reset) {
            resets.push_back(name);
        }
    }

    // Check that no signal or output logic depends on clock/reset.
    // Filter to nodes belonging to this module's instance_path only: the flat DFG
    // contains all modules' nodes, and sibling/ancestor nodes may still have their
    // reset MUXes intact (processing is bottom-up).
    for (const auto& [name, node] : graph.getSignalsMap()) {
        if (node->instance_path != instance_path) continue;
        check_logic_no_clock_reset(node, name, resolved.name, clocks, resets);
    }
    for (const auto& [name, node] : graph.getOutputsMap()) {
        if (node->instance_path != instance_path) continue;
        check_logic_no_clock_reset(node, name, resolved.name, clocks, resets);
    }

}


} // namespace custom_hdl
