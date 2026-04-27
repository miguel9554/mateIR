#include "frontends/systemverilog/passes/domains_propagate_and_check.h"

#include "util/source_loc.h"

#include <format>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
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

ClockId requireClockDomainForLocalClock(
        const Module& module,
        const InstancePath& path,
        const MateIR& ir,
        const ModuleDomainFacts& moduleFacts,
        const std::string& localClockName,
        const std::string& signalName) {
    auto it = moduleFacts.resolved_input_domains.find(localClockName);
    if (it == moduleFacts.resolved_input_domains.end() || !it->second.clock_domain) {
        throw CompilerError(std::format(
            "domains_propagate_and_check: signal '{}' in module '{}' at {} "
            "references unresolved clock domain '{}'",
            signalName, module.name, pathString(path), localClockName));
    }

    ClockId id = *it->second.clock_domain;
    if (id == InvalidClockId || id.value >= ir.clocks.size() ||
            ir.clocks[id.value].id != id) {
        throw CompilerError(std::format(
            "domains_propagate_and_check: signal '{}' in module '{}' at {} "
            "references invalid ClockId {}",
            signalName, module.name, pathString(path), id.value));
    }
    return id;
}

ResetId requireResetDomainForLocalReset(
        const Module& module,
        const InstancePath& path,
        const MateIR& ir,
        const ModuleDomainFacts& moduleFacts,
        const std::string& localResetName,
        const std::string& signalName) {
    auto it = moduleFacts.resolved_input_domains.find(localResetName);
    if (it == moduleFacts.resolved_input_domains.end() || !it->second.reset_domain) {
        throw CompilerError(std::format(
            "domains_propagate_and_check: signal '{}' in module '{}' at {} "
            "references unresolved reset domain '{}'",
            signalName, module.name, pathString(path), localResetName));
    }

    ResetId id = *it->second.reset_domain;
    if (id == InvalidResetId || id.value >= ir.resets.size() ||
            ir.resets[id.value].id != id) {
        throw CompilerError(std::format(
            "domains_propagate_and_check: signal '{}' in module '{}' at {} "
            "references invalid ResetId {}",
            signalName, module.name, pathString(path), id.value));
    }
    return id;
}

bool resetDomainsSortedUnique(const ResetDomains& domains) {
    for (size_t i = 1; i < domains.ids.size(); ++i) {
        if (!(domains.ids[i - 1] < domains.ids[i])) return false;
    }
    return true;
}

void validateSyncTypeIds(
        const MateIR& ir,
        const SyncType& syncType,
        const Module& module,
        const InstancePath& path,
        const std::string& signalName) {
    if (const auto* sync = std::get_if<SyncSignal>(&syncType)) {
        ClockId id = sync->clock_domain;
        if (id == InvalidClockId || id.value >= ir.clocks.size() ||
                ir.clocks[id.value].id != id) {
            throw CompilerError(std::format(
                "domains_propagate_and_check: signal '{}' in module '{}' at {} "
                "has invalid SyncSignal ClockId {}",
                signalName, module.name, pathString(path), id.value));
        }
        if (!resetDomainsSortedUnique(sync->reset_domains)) {
            throw CompilerError(std::format(
                "domains_propagate_and_check: signal '{}' in module '{}' at {} "
                "has unsorted or duplicate reset domains",
                signalName, module.name, pathString(path)));
        }
        for (ResetId resetId : sync->reset_domains.ids) {
            if (resetId == InvalidResetId || resetId.value >= ir.resets.size() ||
                    ir.resets[resetId.value].id != resetId) {
                throw CompilerError(std::format(
                    "domains_propagate_and_check: signal '{}' in module '{}' at {} "
                    "has invalid SyncSignal ResetId {}",
                    signalName, module.name, pathString(path), resetId.value));
            }
        }
        return;
    }

    if (const auto* clock = std::get_if<ClockSignal>(&syncType)) {
        ClockId id = clock->clock_domain;
        if (id == InvalidClockId || id.value >= ir.clocks.size() ||
                ir.clocks[id.value].id != id) {
            throw CompilerError(std::format(
                "domains_propagate_and_check: signal '{}' in module '{}' at {} "
                "has invalid ClockSignal ClockId {}",
                signalName, module.name, pathString(path), id.value));
        }
        return;
    }

    if (const auto* reset = std::get_if<ResetSignal>(&syncType)) {
        ResetId id = reset->reset_domain;
        if (id == InvalidResetId || id.value >= ir.resets.size() ||
                ir.resets[id.value].id != id) {
            throw CompilerError(std::format(
                "domains_propagate_and_check: signal '{}' in module '{}' at {} "
                "has invalid ResetSignal ResetId {}",
                signalName, module.name, pathString(path), id.value));
        }
    }
}

SyncType expectedPortSyncType(
        const Module& module,
        const InstancePath& path,
        const MateIR& ir,
        const ModuleDomainFacts& moduleFacts,
        const Signal& signal) {
    auto it = moduleFacts.ports.find(signal.name);
    if (it == moduleFacts.ports.end()) {
        throw CompilerError(std::format(
            "domains_propagate_and_check: missing port domain fact for '{}' "
            "in module '{}' at {}",
            signal.name, module.name, pathString(path)));
    }

    const LocalPortDomainFact& portFact = it->second;
    switch (portFact.cls) {
        case LocalPortClass::Clock:
            return ClockSignal{
                requireClockDomainForLocalClock(
                    module, path, ir, moduleFacts, signal.name, signal.name),
            };
        case LocalPortClass::Reset:
            return ResetSignal{
                requireResetDomainForLocalReset(
                    module, path, ir, moduleFacts, signal.name, signal.name),
            };
        case LocalPortClass::Async:
            return AsyncSignal{};
        case LocalPortClass::Sync:
            if (!portFact.local_domain_name) {
                throw CompilerError(std::format(
                    "domains_propagate_and_check: sync port '{}' in module '{}' at {} "
                    "has no local clock domain fact",
                    signal.name, module.name, pathString(path)));
            }
            return SyncSignal{
                .clock_domain = requireClockDomainForLocalClock(
                    module, path, ir, moduleFacts, *portFact.local_domain_name, signal.name),
                .reset_domains = {},
            };
    }
    return AsyncSignal{};
}

SyncType mergeSyncTypes(const std::vector<SyncType>& inputs) {
    std::optional<ClockId> clock;
    ResetDomains resets;
    bool sawSync = false;

    for (const SyncType& input : inputs) {
        if (std::holds_alternative<AsyncSignal>(input) ||
                std::holds_alternative<ClockSignal>(input) ||
                std::holds_alternative<ResetSignal>(input)) {
            return AsyncSignal{};
        }

        const auto* sync = std::get_if<SyncSignal>(&input);
        if (!sync) continue;
        if (!clock) {
            clock = sync->clock_domain;
        } else if (*clock != sync->clock_domain) {
            return AsyncSignal{};
        }
        for (ResetId resetId : sync->reset_domains.ids)
            resets.insert(resetId);
        sawSync = true;
    }

    if (!sawSync || !clock) return AsyncSignal{};
    return SyncSignal{.clock_domain = *clock, .reset_domains = std::move(resets)};
}

void setNodeSync(
        std::map<const DFGNode*, SyncType>& nodeSync,
        const DFGNode* node,
        const SyncType& syncType) {
    if (!node) return;
    auto it = nodeSync.find(node);
    if (it == nodeSync.end()) {
        nodeSync[node] = syncType;
    } else if (it->second == syncType) {
        return;
    } else {
        it->second = mergeSyncTypes({it->second, syncType});
    }
}

std::optional<SyncType> nodeSyncType(
        const std::map<const DFGNode*, SyncType>& nodeSync,
        const DFGNode* node) {
    if (!node) return std::nullopt;
    if (node->kind() == DFGOp::CONST) return std::nullopt;
    auto it = nodeSync.find(node);
    if (it == nodeSync.end()) return std::nullopt;
    return it->second;
}

void seedDeclaredInputSyncTypes(
        Module& module,
        const InstancePath& path,
        const MateIR& ir,
        const FrontendDomainFacts& facts,
        std::map<const DFGNode*, SyncType>& nodeSync) {
    const ModuleDomainFacts& moduleFacts = requireFacts(module, path, facts);

    if (!module.pure_combinational) {
        for (auto& [name, input] : module.inputs) {
            input.sync_type = expectedPortSyncType(module, path, ir, moduleFacts, input);
            validateSyncTypeIds(ir, input.sync_type, module, path, input.name);
            for (auto* leaf : signalLeaves(input))
                setNodeSync(nodeSync, leaf, input.sync_type);
        }
    }

    for (auto& sub : module.hierarchyInstantiation)
        seedDeclaredInputSyncTypes(sub, childPath(path, sub.instance_name), ir, facts, nodeSync);
}

void seedFlopSyncTypes(
        const Module& module,
        std::map<const DFGNode*, SyncType>& nodeSync) {
    for (const auto& flop : module.flops) {
        SyncType syncType = SyncSignal{
            .clock_domain = flop.clock_domain,
            .reset_domains = flop.reset_domains,
        };
        for (auto* leaf : flopQLeaves(flop))
            setNodeSync(nodeSync, leaf, syncType);
    }
    for (const auto& sub : module.hierarchyInstantiation)
        seedFlopSyncTypes(sub, nodeSync);
}

std::map<const DFGNode*, SyncType> propagateNodeSyncTypes(
        const DFG& dfg,
        std::map<const DFGNode*, SyncType> nodeSync) {
    bool changed;
    do {
        changed = false;
        for (const auto& nodePtr : dfg.nodes) {
            const DFGNode* node = nodePtr.get();

            std::vector<SyncType> inputs;
            bool hasNonConstInput = false;
            bool missingInput = false;
            DFGTraversal::forEachInput(node, [&](size_t, const DFGOutput& input) {
                if (input.node->kind() == DFGOp::CONST) return;
                hasNonConstInput = true;
                auto syncType = nodeSyncType(nodeSync, input.node);
                if (!syncType) {
                    missingInput = true;
                    return;
                }
                inputs.push_back(*syncType);
            });

            if (missingInput) continue;
            if (!hasNonConstInput && nodeSync.contains(node)) continue;
            SyncType computed = hasNonConstInput ? mergeSyncTypes(inputs) : AsyncSignal{};

            auto it = nodeSync.find(node);
            if (it == nodeSync.end() || it->second != computed) {
                nodeSync[node] = std::move(computed);
                changed = true;
            }
        }
    } while (changed);

    return nodeSync;
}

SyncType syncTypeForSignalLeaves(
        const Signal& signal,
        const std::map<const DFGNode*, SyncType>& nodeSync) {
    std::vector<SyncType> leaves;
    for (auto* leaf : signalLeaves(signal)) {
        auto syncType = nodeSyncType(nodeSync, leaf);
        if (syncType) leaves.push_back(*syncType);
    }
    return mergeSyncTypes(leaves);
}

void assignSignalSyncTypes(
        Module& module,
        const InstancePath& path,
        const MateIR& ir,
        const FrontendDomainFacts& facts,
        const std::map<const DFGNode*, SyncType>& nodeSync) {
    const ModuleDomainFacts& moduleFacts = requireFacts(module, path, facts);

    auto assignInput = [&](Signal& input) {
        if (module.pure_combinational) {
            input.sync_type = syncTypeForSignalLeaves(input, nodeSync);
        } else {
            input.sync_type = expectedPortSyncType(module, path, ir, moduleFacts, input);
        }
        validateSyncTypeIds(ir, input.sync_type, module, path, input.name);
    };

    auto assignDrivenSignal = [&](Signal& signal) {
        SyncType propagated = syncTypeForSignalLeaves(signal, nodeSync);
        if (!module.pure_combinational && moduleFacts.ports.contains(signal.name)) {
            SyncType expected = expectedPortSyncType(module, path, ir, moduleFacts, signal);
            if (std::holds_alternative<AsyncSignal>(propagated) &&
                    !std::holds_alternative<AsyncSignal>(expected)) {
                signal.sync_type = std::move(expected);
            } else {
                signal.sync_type = std::move(propagated);
            }
        } else {
            signal.sync_type = std::move(propagated);
        }
        validateSyncTypeIds(ir, signal.sync_type, module, path, signal.name);
    };

    for (auto& [name, input] : module.inputs) assignInput(input);
    for (auto& [name, output] : module.outputs) assignDrivenSignal(output);
    for (auto& [name, signal] : module.signals) assignDrivenSignal(signal);

    for (auto& sub : module.hierarchyInstantiation)
        assignSignalSyncTypes(sub, childPath(path, sub.instance_name), ir, facts, nodeSync);
}

void validateFlopTriggerFacts(
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
        validateFlopTriggerFacts(sub, childPath(path, sub.instance_name), facts);
}

std::optional<ClockId> synchronizedTargetClockId(
        const Module& module,
        const InstancePath& path,
        const MateIR& ir,
        const FrontendDomainFacts& facts,
        const std::string& portName) {
    const ModuleDomainFacts& moduleFacts = requireFacts(module, path, facts);
    auto portIt = moduleFacts.ports.find(portName);
    if (portIt == moduleFacts.ports.end() || !portIt->second.synchronized_into)
        return std::nullopt;

    return requireClockDomainForLocalClock(
        module, path, ir, moduleFacts, *portIt->second.synchronized_into, portName);
}

void validateCdcForModule(
        const Module& module,
        const InstancePath& path,
        const DFG& topDFG,
        const MateIR& ir,
        const FrontendDomainFacts& facts) {
    auto fwd = buildForwardMap(topDFG);

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
        std::optional<ClockId> synchronizedClock =
            synchronizedTargetClockId(module, path, ir, facts, portName);

        for (auto* leaf : signalLeaves(inputSig)) {
            if (!leaf) continue;
            for (const auto& reached : forwardTraversal(leaf)) {
                if (const auto* sync = std::get_if<SyncSignal>(&inputSig.sync_type)) {
                    if (sync->clock_domain == reached.clock_domain) continue;
                    if (synchronizedClock && *synchronizedClock == reached.clock_domain) continue;
                    throw CompilerError(std::format(
                        "domains_propagate_and_check: module '{}': sync input '{}' "
                        "feeds flop '{}' in a different clock domain - cross-domain violation",
                        module.name, portName, reached.flop_name), reached.d_node);
                }

                if (synchronizedClock && *synchronizedClock == reached.clock_domain) continue;
                throw CompilerError(std::format(
                    "domains_propagate_and_check: module '{}': {} input '{}' "
                    "feeds flop '{}' without a declared synchronizer",
                    module.name, syncKindStr(syncKind(inputSig)),
                    portName, reached.flop_name), reached.d_node);
            }
        }
    }

    for (const auto& sub : module.hierarchyInstantiation)
        validateCdcForModule(sub, childPath(path, sub.instance_name), topDFG, ir, facts);
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

            std::optional<ClockId> childSyncClock =
                synchronizedTargetClockId(sub, subPath, ir, facts, conn.child_port);
            if (childSyncClock) continue;

            std::optional<ClockId> parentSyncClock =
                synchronizedTargetClockId(module, path, ir, facts, *conn.parent_signal_name);
            if (parentSyncClock) continue;
        }

        validateCrossModuleConnections(sub, subPath, ir, facts);
    }
}

} // anonymous namespace

void domainsPropagateAndCheck(MateIR& ir, const FrontendDomainFacts& domainFacts) {
    Module& module = ir.top;
    if (!module.dfg) return;

    validateFlopTriggerFacts(module, {}, domainFacts);

    std::map<const DFGNode*, SyncType> nodeSync;
    seedDeclaredInputSyncTypes(module, {}, ir, domainFacts, nodeSync);
    seedFlopSyncTypes(module, nodeSync);
    nodeSync = propagateNodeSyncTypes(*module.dfg, std::move(nodeSync));

    assignSignalSyncTypes(module, {}, ir, domainFacts, nodeSync);
    validateCdcForModule(module, {}, *module.dfg, ir, domainFacts);
    validateCrossModuleConnections(module, {}, ir, domainFacts);
}

} // namespace mate
