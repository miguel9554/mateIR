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
    } else if (expectedResetAssign->op == DFGOp::SIGNAL) {
        // Check the assignment is the .q value
        if (expectedResetAssign->name != flop_name + ".q") {
            throw CompilerError("Unsupported SIGNAL for reset MUX TRUE: " + expectedResetAssign->name, dNodeDriver->loc);
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
    const auto& dNode = graph.signals.at(dName);
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

void resolveFlops(ResolvedModule& resolved) {
    if (!resolved.dfg || resolved.flops.empty()) {
        return;
    }

    DFG& graph = *resolved.dfg;

    // Connect flop outputs to their .q signals
    for (const auto& output : resolved.outputs) {
        if (resolved.flopsTriggers.contains(output.name)) {
            if (output.type.unpacked_dims.empty()) {
                std::string qName = output.name + ".q";
                DFGNode* qNode = graph.lookupSignal(qName);
                if (qNode) {
                    graph.connectOutput(output.name, qNode);
                }
            } else {
                for (const auto& suffix : generateIndexSuffixes(output.type.unpacked_dims)) {
                    std::string elemOutput = output.name + suffix;
                    std::string elemQ = output.name + suffix + ".q";
                    DFGNode* qNode = graph.lookupSignal(elemQ);
                    if (qNode && graph.outputs.contains(elemOutput)) {
                        graph.connectOutput(elemOutput, qNode);
                    }
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
            auto& output = graph.signals.at(name + ".d");
            const asyncTrigger_t clock = resolved_flops.back().clock;
            const std::optional<asyncTrigger_t> reset = resolved_flops.back().reset;

            // Helper to find exactly one signal
            auto find_unique_input = [&](const std::string& name) -> auto& {
                auto it = std::find_if(resolved.inputs.begin(), resolved.inputs.end(),
                    [&](const auto& input) { return input.name == name; });
                if (it == resolved.inputs.end())
                    throw std::logic_error("No input found matching: " + name);
                if (std::find_if(std::next(it), resolved.inputs.end(),
                        [&](const auto& input) { return input.name == name; }) != resolved.inputs.end())
                    throw std::logic_error("Multiple inputs found matching: " + name);
                return *it;
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

    // Build clock/reset name lists from inputs that were tagged
    std::vector<std::string> clocks;
    std::vector<std::string> resets;
    for (const auto& input : resolved.inputs) {
        if (input.type.kind == ResolvedTypeKind::Clock) {
            clocks.push_back(input.name);
        } else if (input.type.kind == ResolvedTypeKind::Reset) {
            resets.push_back(input.name);
        }
    }

    // Check that no signal or output logic depends on clock/reset
    for (const auto& [name, node] : graph.signals) {
        check_logic_no_clock_reset(node, name, resolved.name, clocks, resets);
    }
    for (const auto& [name, node] : graph.outputs) {
        check_logic_no_clock_reset(node, name, resolved.name, clocks, resets);
    }
}

void propagatePortTypes(ResolvedModule& module) {
    if (!module.dfg) return;

    for (const auto& node : module.dfg->nodes) {
        if (node->op != DFGOp::MODULE) continue;

        // Find the matching submodule definition
        const std::string& moduleType = std::get<std::string>(node->data);
        const ResolvedModule* subDef = nullptr;
        for (const auto& sub : module.hierarchyInstantiation) {
            if (sub.name == moduleType) {
                subDef = &sub;
                break;
            }
        }
        if (!subDef) continue;

        // For each input port of the MODULE node, check if the submodule's
        // corresponding input is tagged as Clock or Reset
        for (size_t i = 0; i < node->input_names.size(); ++i) {
            const std::string& subPortName = node->input_names[i];

            // Find the submodule input with this name
            const ResolvedSignal* subInput = nullptr;
            for (const auto& inp : subDef->inputs) {
                if (inp.name == subPortName) {
                    subInput = &inp;
                    break;
                }
            }
            if (!subInput) continue;

            if (subInput->type.kind != ResolvedTypeKind::Clock &&
                subInput->type.kind != ResolvedTypeKind::Reset)
                continue;

            // The driver of this MODULE input must be a top-level INPUT node
            DFGNode* driverNode = node->in[i].node;
            if (driverNode->op != DFGOp::INPUT) {
                throw CompilerError(std::format(
                    "Submodule '{}' port '{}' is a {} but is driven by {} "
                    "(must be driven by a top-level input port)",
                    node->name, subPortName,
                    subInput->type.kind == ResolvedTypeKind::Clock ? "clock" : "reset",
                    driverNode->str()), node.get());
            }

            // Find the parent's ResolvedSignal for this input and tag it
            bool found = false;
            for (auto& parentInput : module.inputs) {
                if (parentInput.name == driverNode->name) {
                    parentInput.type.kind = subInput->type.kind;
                    found = true;
                    break;
                }
            }
            if (!found) {
                throw CompilerError(std::format(
                    "Parent input '{}' not found in module.inputs",
                    driverNode->name), driverNode);
            }
        }
    }
}

} // namespace custom_hdl
