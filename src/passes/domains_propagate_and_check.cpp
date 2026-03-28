#include "passes/domains_propagate_and_check.h"

#include "util/source_loc.h"

#include <format>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace custom_hdl {

namespace {

// Build forward adjacency: for each node, which (node, arrival_port) pairs use it?
struct FwdEdge {
    const DFGNode* user;
    int input_slot;
};

std::map<const DFGNode*, std::vector<FwdEdge>> buildForwardMap(const DFG& dfg) {
    std::map<const DFGNode*, std::vector<FwdEdge>> fwd;
    for (const auto& node : dfg.nodes) {
        for (int i = 0; i < static_cast<int>(node->in.size()); i++) {
            fwd[node->in[i].node].push_back({node.get(), i});
        }
    }
    return fwd;
}

// Get the flop base name from a .d signal name
std::string flopBaseName(const std::string& dName) {
    return dName.substr(0, dName.size() - 2);
}

const char* syncKindStr(SyncKind k) {
    return k == SyncKind::Sync  ? "Sync"  :
           k == SyncKind::Clock ? "Clock" :
           k == SyncKind::Reset ? "Reset" : "Async";
}

void checkAndPropagateModule(ResolvedModule& mod, const DFG* topDFG) {
    // a. Polarity and b. SyncKind checks — requires flop_resolve to have run
    for (const auto& flop : mod.flops) {
        // Clock checks
        auto clk_it = mod.inputs.find(flop.clock.name);
        if (clk_it != mod.inputs.end()) {
            // b. SyncKind
            if (clk_it->second.sync_kind != SyncKind::Clock) {
                throw CompilerError(std::format(
                    "domains_propagate_and_check: flop '{}' in module '{}': "
                    "clock signal '{}' is not declared as a clock in the domains file",
                    flop.name, mod.name, flop.clock.name));
            }
            // a. Polarity
            if (clk_it->second.clock_edge.has_value() &&
                flop.clock.edge != *clk_it->second.clock_edge) {
                throw CompilerError(std::format(
                    "domains_propagate_and_check: flop '{}' in module '{}': "
                    "clock '{}' polarity mismatch between domains file and always block",
                    flop.name, mod.name, flop.clock.name));
            }
        }

        // Reset checks
        if (flop.reset) {
            auto rst_it = mod.inputs.find(flop.reset->name);
            if (rst_it != mod.inputs.end()) {
                // b. SyncKind
                if (rst_it->second.sync_kind != SyncKind::Reset) {
                    throw CompilerError(std::format(
                        "domains_propagate_and_check: flop '{}' in module '{}': "
                        "reset signal '{}' is not declared as a reset in the domains file",
                        flop.name, mod.name, flop.reset->name));
                }
                // a. Polarity
                if (rst_it->second.clock_edge.has_value() &&
                    flop.reset->edge != *rst_it->second.clock_edge) {
                    throw CompilerError(std::format(
                        "domains_propagate_and_check: flop '{}' in module '{}': "
                        "reset '{}' polarity mismatch between domains file and always block",
                        flop.name, mod.name, flop.reset->name));
                }
            }
        }
    }

    // c. Domain propagation to internal signals and flop types

    // Find clock inputs (single-clock assumption for internal signals)
    ResolvedSignal* singleClockSig = nullptr;
    std::optional<edge_t> singleClockEdge;
    int clockCount = 0;
    for (auto& [name, sig] : mod.inputs) {
        if (sig.sync_kind == SyncKind::Clock) {
            singleClockSig = &sig;
            singleClockEdge = sig.clock_edge;
            ++clockCount;
        }
    }

    // Propagate to internal signals (single-clock modules only)
    if (clockCount == 1) {
        for (auto& [name, sig] : mod.signals) {
            if (sig.sync_kind != SyncKind::Clock && sig.sync_kind != SyncKind::Reset) {
                sig.clock_domain = singleClockSig;
                sig.clock_edge = singleClockEdge;
            }
        }
    }

    // Propagate to flop types
    for (auto& flop : mod.flops) {
        auto clk_it = mod.inputs.find(flop.clock.name);
        if (clk_it != mod.inputs.end()) {
            flop.type.clock_domain = &clk_it->second;
            flop.type.clock_edge = flop.clock.edge;
        }
    }

    // d. CDC fanin validation
    if (!topDFG) return;

    auto fwd = buildForwardMap(*topDFG);

    // Build a map from flop name -> clock domain signal for quick lookup
    std::map<std::string, const ResolvedSignal*> flopClockSig;
    for (const auto& flop : mod.flops) {
        auto clk_it = mod.inputs.find(flop.clock.name);
        if (clk_it != mod.inputs.end()) {
            flopClockSig[flop.name] = &clk_it->second;
        }
    }

    // Forward traversal from an input node, collecting .d signals reached
    auto forwardTraversal = [&](const DFGNode* start)
            -> std::vector<std::pair<std::string, const ResolvedSignal*>> {
        std::vector<std::pair<std::string, const ResolvedSignal*>> reached;
        std::set<const DFGNode*> visited;
        std::vector<const DFGNode*> worklist = {start};

        while (!worklist.empty()) {
            const DFGNode* current = worklist.back();
            worklist.pop_back();

            if (!visited.insert(current).second) continue;

            if (current->op == DFGOp::OUTPUT && current->name.ends_with(".d")) {
                std::string base = flopBaseName(current->name);
                auto it = flopClockSig.find(base);
                if (it != flopClockSig.end()) {
                    reached.push_back({base, it->second});
                }
                continue;
            }

            if (current->op == DFGOp::INPUT && current->name.ends_with(".q")) {
                continue;
            }

            if (auto it = fwd.find(current); it != fwd.end()) {
                for (const auto& edge : it->second) {
                    worklist.push_back(edge.user);
                }
            }
        }
        return reached;
    };

    // Helper: resolve a clock domain name to its ResolvedSignal pointer
    auto findClockSig = [&](const std::string& domainName) -> const ResolvedSignal* {
        auto it = mod.inputs.find(domainName);
        return it != mod.inputs.end() ? &it->second : nullptr;
    };

    // Validate sync domain inputs
    for (const auto& [portName, inputSig] : mod.inputs) {
        if (inputSig.sync_kind != SyncKind::Sync) continue;
        if (!inputSig.dfg_node) continue;

        auto reached = forwardTraversal(inputSig.dfg_node);
        for (const auto& [flopName, flopClk] : reached) {
            if (flopClk == inputSig.clock_domain) continue;
            // Declared CDC crossing via synchronized_into
            auto syncIt = mod.synchronizedSignals.find(portName);
            if (syncIt != mod.synchronizedSignals.end() &&
                flopClk == findClockSig(syncIt->second)) continue;
            throw CompilerError(std::format(
                "domains_propagate_and_check: module '{}': sync input '{}' "
                "feeds flop '{}' in a different clock domain — cross-domain violation",
                mod.name, portName, flopName));
        }
    }

    // Validate async domain inputs
    for (const auto& [portName, inputSig] : mod.inputs) {
        if (inputSig.sync_kind != SyncKind::Async) continue;
        if (!inputSig.dfg_node) continue;

        auto reached = forwardTraversal(inputSig.dfg_node);
        for (const auto& [flopName, flopClk] : reached) {
            // Declared CDC crossing via synchronized_into
            auto syncIt = mod.synchronizedSignals.find(portName);
            if (syncIt != mod.synchronizedSignals.end() &&
                flopClk == findClockSig(syncIt->second)) continue;
            throw CompilerError(std::format(
                "domains_propagate_and_check: module '{}': async input '{}' "
                "feeds flop '{}' without a declared synchronizer",
                mod.name, portName, flopName));
        }
    }

    // e. Cross-module port sync_kind consistency check + asyncPortConnections trimming.
    // For each submodule, every port connection must have matching sync_kind on both
    // the parent signal and the child port. After verification, trim asyncPortConnections
    // to Clock/Reset-only entries (used by the simulator for async port translation).
    for (auto& sub : mod.hierarchyInstantiation) {
        for (const auto& [childPort, parentSignal] : sub.asyncPortConnections) {
            // Pure combinational submodules have no clock domain — skip the check.
            if (sub.pure_combinational) continue;
            // Look up parent signal's sync_kind (input or internal signal)
            SyncKind parentKind;
            auto pit = mod.inputs.find(parentSignal);
            if (pit != mod.inputs.end()) {
                parentKind = pit->second.sync_kind;
            } else {
                auto sit = mod.signals.find(parentSignal);
                if (sit != mod.signals.end()) {
                    parentKind = sit->second.sync_kind;
                } else {
                    throw CompilerError(std::format(
                        "domains_propagate_and_check: module '{}': signal '{}' connected "
                        "to port '{}' of submodule '{}' not found in parent inputs or signals",
                        mod.name, parentSignal, childPort, sub.name));
                }
            }

            // Look up child port's sync_kind
            auto cit = sub.inputs.find(childPort);
            if (cit == sub.inputs.end()) {
                throw CompilerError(std::format(
                    "domains_propagate_and_check: submodule '{}' port '{}' "
                    "not found in inputs",
                    sub.name, childPort));
            }
            SyncKind childKind = cit->second.sync_kind;

            if (parentKind == childKind) continue;

            // Sync → Async: allowed — the child explicitly declares an async/CDC input.
            if (parentKind == SyncKind::Sync && childKind == SyncKind::Async) continue;

            // Reset/Clock → Async: allowed only when the child port declares
            // synchronized_into, meaning it is the input to a synchronizer.
            if (childKind == SyncKind::Async &&
                (parentKind == SyncKind::Reset || parentKind == SyncKind::Clock)) {
                if (sub.synchronizedSignals.contains(childPort)) continue;
                throw CompilerError(std::format(
                    "domains_propagate_and_check: module '{}': {} signal '{}' "
                    "connected to async port '{}' of submodule '{}' without "
                    "synchronized_into declaration on the child port",
                    mod.name, syncKindStr(parentKind), parentSignal,
                    childPort, sub.name));
            }

            // Async → Sync: allowed only if the parent declares synchronized_into for
            // this signal, meaning it has been synchronized before reaching the child.
            if (parentKind == SyncKind::Async && childKind == SyncKind::Sync) {
                if (mod.synchronizedSignals.contains(parentSignal)) continue;
                throw CompilerError(std::format(
                    "domains_propagate_and_check: module '{}': async signal '{}' "
                    "connected to sync port '{}' of submodule '{}' without "
                    "synchronized_into declaration",
                    mod.name, parentSignal, childPort, sub.name));
            }

            throw CompilerError(std::format(
                "domains_propagate_and_check: module '{}': signal '{}' ({}) "
                "connected to port '{}' of submodule '{}' ({}) — sync_kind mismatch",
                mod.name, parentSignal, syncKindStr(parentKind),
                childPort, sub.name, syncKindStr(childKind)));
        }

        // Trim asyncPortConnections to Clock/Reset only
        for (auto it = sub.asyncPortConnections.begin(); it != sub.asyncPortConnections.end(); ) {
            auto cit = sub.inputs.find(it->first);
            if (cit == sub.inputs.end() ||
                (cit->second.sync_kind != SyncKind::Clock &&
                 cit->second.sync_kind != SyncKind::Reset)) {
                it = sub.asyncPortConnections.erase(it);
            } else {
                ++it;
            }
        }
    }
}

} // anonymous namespace

void domainsPropagateAndCheck(ResolvedModule& module) {
    // Bottom-up: process submodules first
    std::function<void(ResolvedModule&)> process = [&](ResolvedModule& mod) {
        for (auto& sub : mod.hierarchyInstantiation)
            process(sub);
        checkAndPropagateModule(mod, module.dfg.get());
    };
    process(module);
}

} // namespace custom_hdl
