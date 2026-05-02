#include "frontends/systemverilog/passes/flop_resolve.h"
#include "mateir/dfg.h"
#include "util/source_loc.h"

#include <algorithm>
#include <format>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace mate {

namespace {

// Return all leaf element names for a flop (expanding unpacked dimensions).
std::vector<std::string> allElements(const std::string& baseName, const Type& type) {
    std::vector<std::string> names;
    for (const auto& suffix : unpackedIndexSuffixes(type)) {
        names.push_back(baseName + suffix);
    }
    return names;
}

std::string inDFG(const InstancePath& instance_path) {
    std::string out;
    for (const auto& elem : instance_path.elems) {
        if (!out.empty()) out += ".";
        out += elem;
    }
    return out;
}

InstancePath childPath(InstancePath path, const std::string& instanceName) {
    path.elems.push_back(instanceName);
    return path;
}

DFGNode* nodeForTrigger(const Module& resolved, const EventTriggerFact& trigger) {
    const auto* input = findInputNode(resolved, trigger.local_signal_name);
    return input ? scalarModuleNode(*input) : nullptr;
}

bool isFlopQValue(const DFGNode* node, const std::string& flop_name) {
    if (node->kind() == DFGOp::INPUT) {
        return node->name == flop_name + ".q";
    }
    if (node->kind() != DFGOp::SLICE) {
        return false;
    }
    auto* source = node->sliceInputs().source.node;
    return source->kind() == DFGOp::INPUT && source->name == flop_name + ".q";
}

int64_t maskForWidth(int width) {
    if (width <= 0) return 0;
    if (width >= 64) return ~0LL;
    return (1LL << width) - 1;
}

std::optional<int64_t> constantValueOfNode(const DFGNode* node) {
    if (node->kind() == DFGOp::CONST) {
        return node->constValue();
    }
    if (node->kind() == DFGOp::SLICE) {
        auto slice = node->sliceInputs();
        auto src = constantValueOfNode(slice.source.node);
        if (!src) return std::nullopt;
        auto hi = constantValueOfNode(slice.high.node);
        auto lo = constantValueOfNode(slice.low.node);
        if (!hi || !lo || *hi < *lo) return std::nullopt;
        int width = static_cast<int>(*hi - *lo + 1);
        return (*src >> *lo) & maskForWidth(width);
    }
    if (node->kind() == DFGOp::CONCAT) {
        int64_t value = 0;
        for (const auto& input : node->concatParts()) {
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
    const std::vector<EventTriggerFact>& triggers,
    const std::string& flop_name,
    const Module& resolved,
    EventTriggerFact& reset,
    EventTriggerFact& clock,
    int& reset_value,
    DFGNode*& functionalLogic)
{
    if (dNodeDriver->concatParts().empty()) {
        throw CompilerError(
            std::format("flop '{}' reset CONCAT has no inputs", flop_name),
            dNodeDriver->loc);
    }

    const DFGNode* firstMux = dNodeDriver->concatParts().front().node;
    if (firstMux->kind() != DFGOp::MUX) {
        throw CompilerError(
            std::format("flop '{}' reset CONCAT input is not a MUX (op={})",
                flop_name, to_string(firstMux->kind())),
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
            flop_name, mux_sel->name, triggers[0].local_signal_name,
            triggers[1].local_signal_name), dNodeDriver->loc);
    }

    std::vector<DFGNode*> functionalParts;
    functionalParts.reserve(dNodeDriver->concatParts().size());

    bool resetIsConst = true;
    bool resetIsQ = true;
    int64_t assembledReset = 0;

    for (const auto& input : dNodeDriver->concatParts()) {
        auto* mux = input.node;
        if (mux->kind() != DFGOp::MUX) {
            throw CompilerError(
                std::format("flop '{}' reset CONCAT contains non-MUX input (op={})",
                    flop_name, to_string(mux->kind())),
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
    const std::vector<EventTriggerFact>& triggers,
    const std::string& flop_name,
    const Module& resolved,
    EventTriggerFact& reset,
    EventTriggerFact& clock,
    int& reset_value,
    DFGNode*& functionalLogic)
{
    if (dNodeDriver->kind() == DFGOp::CONCAT) {
        return extract_reset_from_concat(
            graph, dNodeDriver, triggers, flop_name, resolved,
            reset, clock, reset_value, functionalLogic);
    }

    // With 2 triggers the driver must be a reset MUX, or a CONCAT of reset MUX slices.
    if (dNodeDriver->kind() != DFGOp::MUX) {
        throw CompilerError(std::format(
            "flop '{}' has 2 triggers but its .d driver is neither MUX nor CONCAT-of-MUX (op={})",
            flop_name, to_string(dNodeDriver->kind())), dNodeDriver->loc);
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
    // by the parent's signal node (with a different name), and the input binding tracks
    // that replacement.
    if (nodeForTrigger(resolved, triggers[0]) == mux_sel) {
        reset = triggers[0];
        clock = triggers[1];
    } else if (nodeForTrigger(resolved, triggers[1]) == mux_sel) {
        reset = triggers[1];
        clock = triggers[0];
    } else {
        throw CompilerError(std::format(
            "flop '{}' has 2 triggers but MUX selector '{}' matches neither trigger ('{}', '{}')",
            flop_name, mux_sel->name, triggers[0].local_signal_name,
            triggers[1].local_signal_name), dNodeDriver->loc);
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
    Module& resolved,
    const std::string& flop_name,
    const std::string& instance_path,
    const FlopInfo& flopIn,
    ModuleDomainFacts& moduleFacts,
    DFGNode*& functionalLogic)
{
    auto flop = flopIn;
    DFGNode* dNode = nullptr;
    if (!flopIn.type.unpacked_dims.empty() && flop_name != flopIn.name) {
        dNode = graph.getGraphOutput(instance_path, flop_name + ".d");
    } else {
        dNode = scalarFlopDNode(flopIn);
    }
    if (!dNode) {
        throw CompilerError(std::format(
            "Flop .d node not found: {}",
            instance_path.empty() ? flop_name + ".d" : instance_path + "." + flop_name + ".d"));
    }
    auto dDriver = dNode->driver();
    if (!dDriver) {
        throw CompilerError(std::format(
            "Flop must have single driver: {} has 0",
            instance_path.empty() ? flop_name : instance_path + "." + flop_name + ".d"), dNode->loc);
    }
    auto* dNodeDriver = dDriver->node;
    auto triggerIt = moduleFacts.flop_triggers.find(flopIn.name);
    if (triggerIt == moduleFacts.flop_triggers.end()) {
        throw CompilerError(std::format(
            "flop_resolve: missing trigger facts for flop '{}' in module '{}'",
            flopIn.name, resolved.name));
    }
    const auto& triggers = triggerIt->second.triggers;

    EventTriggerFact clock;
    EventTriggerFact reset;
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

    if (has_reset) {
        flop.reset_value = reset_value;
    }
    moduleFacts.flop_domains[flop_name] = FlopDomainFact{
        .flop_name = flop_name,
        .clock = clock,
        .reset = has_reset
            ? std::optional<EventTriggerFact>(reset)
            : std::nullopt,
        .reset_value = flop.reset_value,
    };
    flop.name = flop_name;
    flop.type.unpacked_dims = {};
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

        if (current->kind() == DFGOp::INPUT) {
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

        DFGTraversal::forEachInput(current, [&](size_t, const DFGOutput& inp) {
            to_visit.push_back(inp.node);
        });
    }
}

} // anonymous namespace

// Forward declarations
static void resolveFlopsForModule(Module& resolved,
                                  DFG& graph,
                                  const InstancePath& instance_path,
                                  FrontendDomainFacts& domainFacts);
static void resolveFlopsRecursive(Module& module,
                                  DFG& topDFG,
                                  const InstancePath& instance_path,
                                  FrontendDomainFacts& domainFacts);

void resolveFlops(Module& module, FrontendDomainFacts& domainFacts) {
    if (!module.dfg) return;
    resolveFlopsRecursive(module, *module.dfg, {}, domainFacts);
}

static void resolveFlopsRecursive(Module& module,
                                  DFG& topDFG,
                                  const InstancePath& instance_path,
                                  FrontendDomainFacts& domainFacts) {
    // Bottom-up: resolve submodules first so their clock/reset types are tagged first
    for (auto& sub : module.hierarchyInstantiation)
        resolveFlopsRecursive(sub, topDFG, childPath(instance_path, sub.instance_name), domainFacts);
    resolveFlopsForModule(module, topDFG, instance_path, domainFacts);
}

static void resolveFlopsForModule(Module& resolved,
                                  DFG& graph,
                                  const InstancePath& instance_path,
                                  FrontendDomainFacts& domainFacts) {
    const std::string dfgInstancePath = inDFG(instance_path);
    ModuleDomainFacts& privateFacts = domainFacts.getOrCreate({instance_path, resolved.name});
    if (resolved.flops.empty()) {
        return;
    }

    // Connect flop output port nodes to their .q INPUT nodes.
    // Uses output bindings directly since after DFG inlining submodule output
    // ports may not be in the flat top DFG's outputs map (only .d nodes are adopted).
    forEachOutputNode(resolved, [&](ModuleNode& output) {
        const std::string& outName = output.name;
        if (!privateFacts.flop_triggers.contains(outName)) return;
        if (output.type.unpacked_dims.empty()) {
            DFGNode* qNode = graph.getGraphInput(dfgInstancePath, outName + ".q");
            if (qNode) {
                graph.connectDriver(scalarModuleNode(output), {qNode, 0});
            }
        } else {
            auto suffixes = unpackedIndexSuffixes(output.type);
            const auto& leaves = moduleNodeLeaves(output);
            for (size_t i = 0; i < suffixes.size(); ++i) {
                const auto& suffix = suffixes[i];
                DFGNode* qNode = graph.getGraphInput(dfgInstancePath, outName + suffix + ".q");
                DFGNode* outNode = i < leaves.size()
                    ? leaves[i]
                    : graph.getGraphOutput(dfgInstancePath, outName + suffix);
                if (qNode && outNode) {
                    graph.connectDriver(outNode, {qNode, 0});
                }
            }
        }
    });

    // Extract clock/reset info and validate functional logic
    std::vector<FlopInfo> resolved_flops;
    for (const auto& flop : resolved.flops) {
        for (const auto& name : allElements(flop.name, flop.type)) {
            DFGNode* functional_logic;
            resolved_flops.push_back(
                extractFlopClockAndReset(
                    graph, resolved, name, dfgInstancePath, flop, privateFacts, functional_logic));
            // After flop_resolve, every FlopInfo represents one physical flop leaf.
            {
                auto* dNode = graph.getGraphOutput(dfgInstancePath, name + ".d");
                auto* qNode = graph.getGraphInput(dfgInstancePath, name + ".q");
                resolved_flops.back().binding = FlopBinding{
                    .d_leaves = {dNode},
                    .q_leaves = {qNode},
                };
            }
            DFGNode* output = scalarFlopDNode(resolved_flops.back());

            const FlopDomainFact& flopDomain = privateFacts.flop_domains.at(name);
            const EventTriggerFact& clock = flopDomain.clock;
            const std::optional<EventTriggerFact>& reset = flopDomain.reset;

            // Helper to find exactly one signal
            auto find_unique_input = [&](const std::string& sig_name, const char* role) -> ModuleNode& {
                auto* input = findInputNode(resolved, sig_name);
                if (!input)
                    throw CompilerError(std::format(
                        "flop_resolve: flop '{}' in module '{}' references {} signal '{}' "
                        "which is not an input port",
                        name, resolved.name, role, sig_name));
                return *input;
            };

            // Validate clock/reset are input ports; final SyncType assignment happens
            // after global domain resolution in domains_propagate_and_check.
            find_unique_input(clock.local_signal_name, "clock");  // validate it's an input port
            if (reset) {
                find_unique_input(reset->local_signal_name, "reset");  // validate it's an input port
            }

            // Connect functional logic to the flop's .d signal
            // If there was reset, this removes the reset MUX
            graph.connectDriver(output, functional_logic);
        }
    }
    resolved.flops = resolved_flops;

    // Build clock/reset name lists from RTL trigger facts. Internal clock/reset
    // roles are inferred from flops, not from YAML port classifications.
    std::vector<std::string> clocks;
    std::vector<std::string> resets;
    for (const auto& [flopName, flopDomain] : privateFacts.flop_domains) {
        if (std::ranges::find(clocks, flopDomain.clock.local_signal_name) == clocks.end())
            clocks.push_back(flopDomain.clock.local_signal_name);
        if (flopDomain.reset &&
                std::ranges::find(resets, flopDomain.reset->local_signal_name) == resets.end()) {
            resets.push_back(flopDomain.reset->local_signal_name);
        }
    }

    // Check that module-visible logic and resolved flop functional .d logic do
    // not depend on clock/reset signals.
    forEachDrivenNode(resolved, [&](const ModuleNode& signal) {
        for (const auto& leaf : moduleNodeLeafRefs(signal)) {
            if (!leaf.node) continue;
            check_logic_no_clock_reset(leaf.node, leaf.leaf_name, resolved.name, clocks, resets);
        }
    });
    for (const auto& flop : resolved.flops) {
        for (size_t i = 0; i < flop.binding.d_leaves.size(); ++i) {
            auto* dLeaf = flop.binding.d_leaves[i];
            if (!dLeaf) continue;
            const std::string rootName = flop.binding.d_leaves.size() == 1
                ? flop.name + ".d"
                : std::format("{}[{}].d", flop.name, i);
            check_logic_no_clock_reset(dLeaf, rootName, resolved.name, clocks, resets);
        }
    }

}


} // namespace mate
