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

bool extract_reset(
    const DFGNode* dNodeDriver,
    const std::vector<asyncTrigger_t>& triggers,
    const std::string& flop_name,
    asyncTrigger_t& reset,
    asyncTrigger_t& clock,
    int& reset_value,
    DFGNode*& functionalLogic)
{
    // Check if MUX, if not, no reset
    if (dNodeDriver->op != DFGOp::MUX) {
        return false;
    }

    auto* mux_sel = dNodeDriver->in[0].node;
    auto* mux_true = dNodeDriver->in[1].node;
    auto* mux_else = dNodeDriver->in[2].node;
    DFGNode* expectedResetNode = mux_sel;
    DFGNode* expectedResetAssign;

    // Try to match selector to one of the triggers, which should be the reset. The other trigger is the clock.
    // If no match, MUX should be a functional one.
    const std::string& reset_name = expectedResetNode->name;
    if (triggers[0].name == reset_name) {
        reset = triggers[0];
        clock = triggers[1];
    } else if (triggers[1].name == reset_name) {
        reset = triggers[1];
        clock = triggers[0];
    } else {
        return false;
    }

    // Assign the expected reset and functional branches
    expectedResetAssign = reset.edge == edge_t::POSEDGE ? mux_true : mux_else;
    functionalLogic = reset.edge == edge_t::POSEDGE ? mux_else : mux_true;

    // Check the reset assignment is either a CONSTANT (has reset) or its .q value (NO reset)
    bool has_reset;
    if (expectedResetAssign->op == DFGOp::CONST) {
        reset_value = std::get<int64_t>(expectedResetAssign->data);
        has_reset = true;
    } else if (expectedResetAssign->op == DFGOp::INPUT) {
        // Check the assignment is the .q value (now an INPUT node after .q promotion)
        if (expectedResetAssign->name != flop_name + ".q") {
            throw CompilerError("Unsupported INPUT for reset MUX TRUE: " + expectedResetAssign->name, dNodeDriver->loc);
        }
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
    const FlopInfo& flopIn,
    DFGNode*& functionalLogic)
{
    auto flop = flopIn;
    const std::string dName = flop_name + ".d";
    DFGNode* dNode = flopIn.d_node ? flopIn.d_node : graph.getOutputNode(dName);
    auto* dNodeDriver = dNode->in[0].node;
    const auto& triggers = resolved.flopsTriggers.at(flopIn.name);

    if (dNode->in.size() != 1) {
        throw CompilerError(std::format(
            "Flop must have single driver: {} has {}", dName, dNode->in.size()), dNode->loc);
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
            dNodeDriver, triggers, flopIn.name,
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
static void resolveFlopsForModule(ResolvedModule& resolved, DFG& graph);
static void resolveFlopsRecursive(ResolvedModule& resolved, DFG& topGraph);

// After DFG inlining there are no MODULE nodes, so propagatePortTypes can't work.
// Propagate Clock/Reset type from submodule inputs to parent inputs by name matching.
// (Works for wildcard .* and any same-name port connections; explicit different-name
// connections would require additional port mapping info.)
static void propagateHierarchyPortTypes(ResolvedModule& module) {
    for (const auto& sub : module.hierarchyInstantiation) {
        for (const auto& [portName, subInput] : sub.inputs) {
            if (subInput.type.kind != ResolvedTypeKind::Clock &&
                subInput.type.kind != ResolvedTypeKind::Reset) continue;
            if (auto it = module.inputs.find(portName); it != module.inputs.end()) {
                it->second.type.kind = subInput.type.kind;
            }
        }
    }
}

// Recursively process all submodule levels bottom-up, all using the flat top-level DFG.
static void resolveFlopsRecursive(ResolvedModule& resolved, DFG& topGraph) {
    for (auto& sub : resolved.hierarchyInstantiation)
        resolveFlopsRecursive(sub, topGraph);
    resolveFlopsForModule(resolved, topGraph);
}

void resolveFlops(ResolvedModule& module) {
    if (!module.dfg) return;
    // Recurse bottom-up so sub-module clock/reset types are tagged first
    for (auto& sub : module.hierarchyInstantiation)
        resolveFlopsRecursive(sub, *module.dfg);
    resolveFlopsForModule(module, *module.dfg);
    // Propagate Clock/Reset types from submodule inputs to parent inputs
    propagateHierarchyPortTypes(module);
}

static void resolveFlopsForModule(ResolvedModule& resolved, DFG& graph) {
    if (resolved.flops.empty()) {
        return;
    }

    // Connect flop output port nodes to their .q INPUT nodes.
    // Uses dfg_node pointer directly since after DFG inlining, submodule output ports
    // may not be in the flat top DFG's outputs map (only .d nodes are adopted).
    for (auto& [outName, output] : resolved.outputs) {
        if (!resolved.flopsTriggers.contains(outName)) continue;
        if (output.type.unpacked_dims.empty()) {
            std::string qName = outName + ".q";
            DFGNode* qNode = graph.getInputNode(qName);
            if (qNode && output.dfg_node) {
                output.dfg_node->in = {{qNode, 0}};
            }
        } else {
            for (const auto& suffix : generateIndexSuffixes(output.type.unpacked_dims)) {
                std::string elemQ = outName + suffix + ".q";
                DFGNode* qNode = graph.getInputNode(elemQ);
                // Individual element output nodes may also be in dfg outputs map
                // or just exist as nodes in the flat DFG; use the map if available.
                DFGNode* outNode = graph.getOutputNode(outName + suffix);
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
                extractFlopClockAndReset(graph, resolved, name, flop, functional_logic));
            // Set per-element d_node/q_node (the copy from flopIn may be null for vectorized)
            {
                const std::string dname = name + ".d";
                const std::string qname = name + ".q";
                resolved_flops.back().d_node = graph.getOutputNode(dname);
                resolved_flops.back().q_node = graph.getInputNode(qname);
            }
            DFGNode* output = resolved_flops.back().d_node;
            const asyncTrigger_t clock = resolved_flops.back().clock;
            const std::optional<asyncTrigger_t> reset = resolved_flops.back().reset;

            // Helper to find exactly one signal
            auto find_unique_input = [&](const std::string& name) -> ResolvedSignal& {
                auto it = resolved.inputs.find(name);
                if (it == resolved.inputs.end())
                    throw std::logic_error("No input found matching: " + name);
                return it->second;
            };

            // Set clock and reset types
            find_unique_input(clock.name).type.kind = ResolvedTypeKind::Clock;
            if (reset) {
                find_unique_input(reset->name).type.kind = ResolvedTypeKind::Reset;
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
        if (input.type.kind == ResolvedTypeKind::Clock) {
            clocks.push_back(name);
        } else if (input.type.kind == ResolvedTypeKind::Reset) {
            resets.push_back(name);
        }
    }

    // Check that no signal or output logic depends on clock/reset
    for (const auto& [name, node] : graph.getSignalsMap()) {
        check_logic_no_clock_reset(node, name, resolved.name, clocks, resets);
    }
    for (const auto& [name, node] : graph.getOutputsMap()) {
        check_logic_no_clock_reset(node, name, resolved.name, clocks, resets);
    }
}


} // namespace custom_hdl
