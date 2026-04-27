#include "frontends/systemverilog/passes/cdc_check.h"

#include "util/source_loc.h"

#include <format>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace mate {

namespace {

struct FwdEdge {
    const DFGNode* user;
    int input_slot;
};

struct ReachedFlop {
    std::string flop_name;
    ClockId clock_domain;
    const DFGNode* d_node;
};

std::map<const DFGNode*, std::vector<FwdEdge>> buildForwardMap(const DFG& dfg) {
    std::map<const DFGNode*, std::vector<FwdEdge>> fwd;
    for (const auto& node : dfg.nodes) {
        DFGTraversal::forEachInput(node.get(), [&](size_t i, const DFGOutput& input) {
            fwd[input.node].push_back({node.get(), static_cast<int>(i)});
        });
    }
    return fwd;
}

std::string flopBaseName(const std::string& dName) {
    return dName.substr(0, dName.size() - 2);
}

InstancePath childPath(InstancePath path, const std::string& instanceName) {
    path.elems.push_back(instanceName);
    return path;
}

std::string dfgInstancePath(const InstancePath& path) {
    std::string out;
    for (const auto& elem : path.elems) {
        if (!out.empty()) out += ".";
        out += elem;
    }
    return out;
}

std::string pathString(const InstancePath& path) {
    std::string out = dfgInstancePath(path);
    return out.empty() ? "<top>" : out;
}

const char* syncKindStr(SyncKind k) {
    return k == SyncKind::Sync  ? "Sync"  :
           k == SyncKind::Clock ? "Clock" :
           k == SyncKind::Reset ? "Reset" : "Async";
}

const ModuleDomainFacts& requireFacts(
        const Module& module,
        const InstancePath& path,
        const FrontendDomainFacts& facts) {
    ModuleOccurrenceKey key{path, module.name};
    const auto* moduleFacts = facts.find(key);
    if (!moduleFacts) {
        throw CompilerError(std::format(
            "domains_propagate_and_check: missing domain facts for module '{}' at {}",
            module.name, pathString(path)));
    }
    return *moduleFacts;
}

void validateFlopTriggerFactsForModule(
        const Module& module,
        const InstancePath& path,
        const FrontendDomainFacts& facts) {
    const ModuleDomainFacts& moduleFacts = requireFacts(module, path, facts);
    for (const auto& flop : module.flops) {
        auto factIt = moduleFacts.flop_domains.find(flop.name);
        if (factIt == moduleFacts.flop_domains.end()) continue;
        const FlopDomainFact& fact = factIt->second;

        auto portIt = moduleFacts.ports.find(fact.clock.local_signal_name);
        if (portIt == moduleFacts.ports.end() || portIt->second.cls != LocalPortClass::Clock) {
            throw CompilerError(std::format(
                "domains_propagate_and_check: flop '{}' in module '{}' at {}: "
                "clock signal '{}' is not declared as a clock in the domains file",
                flop.name, module.name, pathString(path), fact.clock.local_signal_name),
                fact.clock.loc);
        }
        if (portIt->second.edge && fact.clock.edge != *portIt->second.edge) {
            throw CompilerError(std::format(
                "domains_propagate_and_check: flop '{}' in module '{}' at {}: "
                "clock '{}' polarity mismatch between domains file and always block",
                flop.name, module.name, pathString(path), fact.clock.local_signal_name),
                fact.clock.loc);
        }

        if (fact.reset) {
            auto resetIt = moduleFacts.ports.find(fact.reset->local_signal_name);
            if (resetIt == moduleFacts.ports.end() || resetIt->second.cls != LocalPortClass::Reset) {
                throw CompilerError(std::format(
                    "domains_propagate_and_check: flop '{}' in module '{}' at {}: "
                    "reset signal '{}' is not declared as a reset in the domains file",
                    flop.name, module.name, pathString(path), fact.reset->local_signal_name),
                    fact.reset->loc);
            }
            if (resetIt->second.edge && fact.reset->edge != *resetIt->second.edge) {
                throw CompilerError(std::format(
                    "domains_propagate_and_check: flop '{}' in module '{}' at {}: "
                    "reset '{}' polarity mismatch between domains file and always block",
                    flop.name, module.name, pathString(path), fact.reset->local_signal_name),
                    fact.reset->loc);
            }
        }
    }

    for (const auto& sub : module.hierarchyInstantiation)
        validateFlopTriggerFactsForModule(sub, childPath(path, sub.instance_name), facts);
}

void validateCdcForModule(
        const Module& module,
        const InstancePath& path,
        const DFG& topDFG,
        const FrontendDomainFacts& facts) {
    auto fwd = buildForwardMap(topDFG);
    const ModuleDomainFacts& moduleFacts = requireFacts(module, path, facts);

    std::map<std::string, ClockId> flopClock;
    for (const auto& flop : module.flops)
        flopClock[flop.name] = flop.clock_domain;

    auto forwardTraversal = [&](const DFGNode* start) -> std::vector<ReachedFlop> {
        std::vector<ReachedFlop> reached;
        std::set<const DFGNode*> visited;
        std::vector<const DFGNode*> worklist = {start};

        while (!worklist.empty()) {
            const DFGNode* current = worklist.back();
            worklist.pop_back();

            if (!visited.insert(current).second) continue;

            if (current->kind() == DFGOp::OUTPUT && current->name.ends_with(".d") &&
                    current->instance_path == dfgInstancePath(path)) {
                std::string base = flopBaseName(current->name);
                auto it = flopClock.find(base);
                if (it != flopClock.end())
                    reached.push_back({base, it->second, current});
                continue;
            }

            if (current->kind() == DFGOp::INPUT && current->name.ends_with(".q")) continue;

            if (auto it = fwd.find(current); it != fwd.end()) {
                for (const auto& edge : it->second)
                    worklist.push_back(edge.user);
            }
        }
        return reached;
    };

    for (const auto& [portName, inputSig] : module.inputs) {
        for (auto* leaf : signalLeaves(inputSig)) {
            if (!leaf) continue;
            for (const auto& reached : forwardTraversal(leaf)) {
                if (moduleFacts.cdc.synchronizer_flops.contains(reached.flop_name))
                    continue;

                if (const auto* sync = std::get_if<SyncSignal>(&inputSig.sync_type)) {
                    if (sync->clock_domain == reached.clock_domain) continue;
                    throw CompilerError(std::format(
                        "domains_propagate_and_check: module '{}': sync input '{}' "
                        "feeds flop '{}' in a different clock domain - cross-domain violation",
                        module.name, portName, reached.flop_name), reached.d_node);
                }

                throw CompilerError(std::format(
                    "domains_propagate_and_check: module '{}': {} input '{}' "
                    "feeds flop '{}' without a declared synchronizer",
                    module.name, syncKindStr(syncKind(inputSig)),
                    portName, reached.flop_name), reached.d_node);
            }
        }
    }

    for (const auto& sub : module.hierarchyInstantiation)
        validateCdcForModule(sub, childPath(path, sub.instance_name), topDFG, facts);
}

const Signal* findParentSignal(const Module& module, const std::string& name) {
    if (auto it = module.inputs.find(name); it != module.inputs.end()) return &it->second;
    if (auto it = module.signals.find(name); it != module.signals.end()) return &it->second;
    if (auto it = module.outputs.find(name); it != module.outputs.end()) return &it->second;
    return nullptr;
}

void validateCrossModuleConnections(
        Module& module,
        const InstancePath& path,
        const MateIR& ir,
        const FrontendDomainFacts& facts) {
    const ModuleDomainFacts& moduleFacts = requireFacts(module, path, facts);

    for (auto& sub : module.hierarchyInstantiation) {
        InstancePath subPath = childPath(path, sub.instance_name);

        for (const auto& conn : moduleFacts.child_input_connections) {
            if (conn.child_instance_path != subPath ||
                    conn.child_module_name != sub.name ||
                    conn.expr_kind != ConnectionExprKind::SimpleIdentifier ||
                    !conn.parent_signal_name) {
                continue;
            }

            const Signal* parentSignal = findParentSignal(module, *conn.parent_signal_name);
            if (!parentSignal) {
                throw CompilerError(std::format(
                    "domains_propagate_and_check: module '{}': signal '{}' connected "
                    "to port '{}' of submodule '{}' not found in parent",
                    module.name, *conn.parent_signal_name, conn.child_port, sub.name),
                    conn.loc);
            }

            auto childIt = sub.inputs.find(conn.child_port);
            if (childIt == sub.inputs.end()) {
                throw CompilerError(std::format(
                    "domains_propagate_and_check: submodule '{}' port '{}' not found in inputs",
                    sub.name, conn.child_port), conn.loc);
            }

            if (sub.pure_combinational) continue;
            const Signal& childSignal = childIt->second;
            if (parentSignal->sync_type == childSignal.sync_type) continue;
            if (const auto* parentSync = std::get_if<SyncSignal>(&parentSignal->sync_type)) {
                if (const auto* childSync = std::get_if<SyncSignal>(&childSignal.sync_type)) {
                    if (parentSync->clock_domain == childSync->clock_domain) continue;
                }
            }
            if (std::holds_alternative<ClockSignal>(childSignal.sync_type) &&
                    std::holds_alternative<ClockSignal>(parentSignal->sync_type)) {
                continue;
            }
            if (std::holds_alternative<ResetSignal>(childSignal.sync_type) &&
                    std::holds_alternative<ResetSignal>(parentSignal->sync_type)) {
                continue;
            }

            if (module.inputs.contains(*conn.parent_signal_name) &&
                    std::holds_alternative<AsyncSignal>(parentSignal->sync_type) &&
                    !std::holds_alternative<AsyncSignal>(childSignal.sync_type)) {
                throw CompilerError(std::format(
                    "domains_propagate_and_check: cross-module sync type mismatch: "
                    "parent input '{}' in module '{}' is Async but child port '{}.{}' "
                    "in module '{}' is {}",
                    *conn.parent_signal_name, module.name,
                    sub.instance_name.empty() ? sub.name : sub.instance_name,
                    conn.child_port, sub.name, syncKindStr(syncKind(childSignal))),
                    conn.loc);
            }
        }

        validateCrossModuleConnections(sub, subPath, ir, facts);
    }
}

} // anonymous namespace

void validateFlopTriggerFacts(
        const Module& top,
        const FrontendDomainFacts& domainFacts) {
    validateFlopTriggerFactsForModule(top, {}, domainFacts);
}

void checkCdcAndCrossModuleConnections(
        Module& top,
        const DFG& topDFG,
        const MateIR& ir,
        const FrontendDomainFacts& domainFacts,
        const SyncDomainAnalysis& analysis) {
    (void)analysis;
    validateCdcForModule(top, {}, topDFG, domainFacts);
    validateCrossModuleConnections(top, {}, ir, domainFacts);
}

} // namespace mate
