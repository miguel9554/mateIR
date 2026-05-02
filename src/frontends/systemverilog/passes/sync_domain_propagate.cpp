#include "frontends/systemverilog/passes/sync_domain_propagate.h"

#include "util/source_loc.h"

#include <format>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace mate {

namespace {

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

ClockId requireResolvedTopClock(
        const MateIR& ir,
        const FrontendDomainFacts& facts,
        const std::string& domainName,
        const std::string& signalName) {
    if (!facts.top_inputs) {
        throw CompilerError(
            "domains_propagate_and_check: top input domain facts are missing");
    }
    auto it = facts.top_inputs->resolved_clocks.find(domainName);
    if (it == facts.top_inputs->resolved_clocks.end()) {
        throw CompilerError(std::format(
            "domains_propagate_and_check: top-level signal '{}' references "
            "unresolved clock domain '{}'",
            signalName, domainName));
    }
    ClockId id = it->second.clock_domain;
    if (id == InvalidClockId || id.value >= ir.clocks.size() ||
            ir.clocks[id.value].id != id) {
        throw CompilerError(std::format(
            "domains_propagate_and_check: top-level signal '{}' references "
            "invalid ClockId {}",
            signalName, id.value));
    }
    return id;
}

ResetId requireResolvedTopReset(
        const MateIR& ir,
        const FrontendDomainFacts& facts,
        const std::string& resetName,
        const std::string& signalName) {
    if (!facts.top_inputs) {
        throw CompilerError(
            "domains_propagate_and_check: top input domain facts are missing");
    }
    auto it = facts.top_inputs->resolved_resets.find(resetName);
    if (it == facts.top_inputs->resolved_resets.end()) {
        throw CompilerError(std::format(
            "domains_propagate_and_check: top-level signal '{}' references "
            "unresolved reset domain '{}'",
            signalName, resetName));
    }
    ResetId id = it->second.reset_domain;
    if (id == InvalidResetId || id.value >= ir.resets.size() ||
            ir.resets[id.value].id != id) {
        throw CompilerError(std::format(
            "domains_propagate_and_check: top-level signal '{}' references "
            "invalid ResetId {}",
            signalName, id.value));
    }
    return id;
}

std::optional<SyncType> topPortSyncType(
        const MateIR& ir,
        const FrontendDomainFacts& facts,
        const Signal& signal) {
    if (!facts.top_inputs) return std::nullopt;

    for (const auto& [domainName, clock] : facts.top_inputs->clocks) {
        if (clock.input_port == signal.name) {
            return ClockSignal{
                requireResolvedTopClock(ir, facts, domainName, signal.name),
            };
        }
    }
    for (const auto& [resetName, reset] : facts.top_inputs->resets) {
        if (reset.signal_name == signal.name) {
            return ResetSignal{
                requireResolvedTopReset(ir, facts, resetName, signal.name),
            };
        }
    }
    if (auto syncIt = facts.top_inputs->sync_inputs.find(signal.name);
            syncIt != facts.top_inputs->sync_inputs.end()) {
        return SyncSignal{
            .clock_domain = requireResolvedTopClock(
                ir, facts, syncIt->second.clock_domain_name, signal.name),
            .reset_domains = {},
        };
    }
    if (facts.top_inputs->async_inputs.contains(signal.name)) {
        return AsyncSignal{};
    }
    return std::nullopt;
}

SyncType expectedPortSyncType(
        const Module& module,
        const InstancePath& path,
        const MateIR& ir,
        const FrontendDomainFacts& facts,
        const ModuleDomainFacts& moduleFacts,
        const Signal& signal) {
    if (path.elems.empty()) {
        auto topType = topPortSyncType(ir, facts, signal);
        if (!topType) {
            throw CompilerError(std::format(
                "domains_propagate_and_check: missing top-level input domain for '{}' "
                "in module '{}'",
                signal.name, module.name));
        }
        return *topType;
    }

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
    bool sawStatic = false;

    for (const SyncType& input : inputs) {
        if (std::holds_alternative<StaticSignal>(input)) {
            sawStatic = true;
            continue;
        }
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

    if (!sawSync || !clock) {
        return sawStatic ? SyncType{StaticSignal{}} : SyncType{AsyncSignal{}};
    }
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

    for (auto& [name, input] : module.inputs) {
        if (!path.elems.empty()) continue;

        input.sync_type = expectedPortSyncType(module, path, ir, facts, moduleFacts, input);
        validateSyncTypeIds(ir, input.sync_type, module, path, input.name);
        for (auto* leaf : signalLeaves(input))
            setNodeSync(nodeSync, leaf, input.sync_type);
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
            SyncType computed = hasNonConstInput ? mergeSyncTypes(inputs) : StaticSignal{};

            auto it = nodeSync.find(node);
            if (it == nodeSync.end() || it->second != computed) {
                nodeSync[node] = std::move(computed);
                changed = true;
            }
        }
    } while (changed);

    return nodeSync;
}

SyncType syncTypeForLeaves(
        const std::vector<DFGNode*>& leaves,
        const std::map<const DFGNode*, SyncType>& nodeSync) {
    std::vector<SyncType> leafSyncTypes;
    for (auto* leaf : leaves) {
        auto syncType = nodeSyncType(nodeSync, leaf);
        if (syncType) leafSyncTypes.push_back(*syncType);
    }
    return mergeSyncTypes(leafSyncTypes);
}

bool hasNonConstFanin(const DFGNode* node, std::set<const DFGNode*>& visited) {
    if (!node || !visited.insert(node).second) return false;
    if (node->kind() == DFGOp::CONST) return false;

    bool found = false;
    bool hasInput = false;
    DFGTraversal::forEachInput(node, [&](size_t, const DFGOutput& input) {
        hasInput = true;
        if (found) return;
        if (hasNonConstFanin(input.node, visited)) found = true;
    });
    if (!hasInput) return node->kind() == DFGOp::INPUT;
    return found;
}

bool hasNonConstFanin(const std::vector<DFGNode*>& leaves) {
    for (auto* leaf : leaves) {
        std::set<const DFGNode*> visited;
        if (hasNonConstFanin(leaf, visited)) return true;
    }
    return false;
}

std::optional<SyncType> computeSyncTypeFromFanin(
        const DFGNode* node,
        const std::map<const DFGNode*, SyncType>& nodeSync,
        std::map<const DFGNode*, std::optional<SyncType>>& memo) {
    if (!node || node->kind() == DFGOp::CONST) return std::nullopt;
    if (auto it = memo.find(node); it != memo.end()) return it->second;

    bool hasInput = false;
    std::vector<SyncType> inputs;
    DFGTraversal::forEachInput(node, [&](size_t, const DFGOutput& input) {
        if (input.node->kind() == DFGOp::CONST) return;
        hasInput = true;
        auto inputSync = computeSyncTypeFromFanin(input.node, nodeSync, memo);
        if (inputSync) inputs.push_back(*inputSync);
    });

    std::optional<SyncType> result;
    if (!hasInput) {
        if (node->kind() != DFGOp::INPUT) {
            result = std::nullopt;
        } else if (auto seed = nodeSync.find(node); seed != nodeSync.end()) {
            result = seed->second;
        } else {
            result = StaticSignal{};
        }
    } else if (inputs.empty()) {
        result = std::nullopt;
    } else {
        result = mergeSyncTypes(inputs);
    }
    memo[node] = result;
    return result;
}

std::optional<SyncType> computeSyncTypeFromFanin(
        const std::vector<DFGNode*>& leaves,
        const std::map<const DFGNode*, SyncType>& nodeSync) {
    std::map<const DFGNode*, std::optional<SyncType>> memo;
    std::vector<SyncType> leafSyncTypes;
    for (const auto* leaf : leaves) {
        auto syncType = computeSyncTypeFromFanin(leaf, nodeSync, memo);
        if (syncType) leafSyncTypes.push_back(*syncType);
    }
    if (leafSyncTypes.empty()) return std::nullopt;
    return mergeSyncTypes(leafSyncTypes);
}

FlopDInputDomain flopDInputDomainForLeaves(
        const std::vector<DFGNode*>& leaves,
        const std::map<const DFGNode*, SyncType>& nodeSync) {
    if (!hasNonConstFanin(leaves)) {
        return FlopDInputDomain{
            .sync_type = StaticSignal{},
        };
    }

    auto syncType = computeSyncTypeFromFanin(leaves, nodeSync);
    if (syncType) {
        return FlopDInputDomain{
            .sync_type = *syncType,
        };
    }
    return FlopDInputDomain{
        .sync_type = std::nullopt,
    };
}

SyncType syncTypeForSignalLeaves(
        const Signal& signal,
        const std::map<const DFGNode*, SyncType>& nodeSync) {
    return syncTypeForLeaves(signalLeaves(signal), nodeSync);
}

void assignSignalSyncTypes(
        Module& module,
        const InstancePath& path,
        const MateIR& ir,
        const FrontendDomainFacts& facts,
        const std::map<const DFGNode*, SyncType>& nodeSync) {
    const ModuleDomainFacts& moduleFacts = requireFacts(module, path, facts);

    auto assignInput = [&](Signal& input) {
        bool useDeclared = path.elems.empty();
        if (!useDeclared) {
            auto portIt = moduleFacts.ports.find(input.name);
            useDeclared = portIt != moduleFacts.ports.end() &&
                (portIt->second.cls == LocalPortClass::Clock ||
                 portIt->second.cls == LocalPortClass::Reset);
        }
        if (useDeclared) {
            input.sync_type = expectedPortSyncType(module, path, ir, facts, moduleFacts, input);
        } else {
            input.sync_type = syncTypeForSignalLeaves(input, nodeSync);
        }
        validateSyncTypeIds(ir, input.sync_type, module, path, input.name);
    };

    auto assignDrivenSignal = [&](Signal& signal) {
        SyncType propagated = syncTypeForSignalLeaves(signal, nodeSync);
        if (moduleFacts.ports.contains(signal.name)) {
            SyncType expected = expectedPortSyncType(module, path, ir, facts, moduleFacts, signal);
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

    forEachInputNode(module, assignInput);
    forEachDrivenNode(module, assignDrivenSignal);

    for (auto& sub : module.hierarchyInstantiation)
        assignSignalSyncTypes(sub, childPath(path, sub.instance_name), ir, facts, nodeSync);
}

void collectFlopDSyncTypes(
        const Module& module,
        const std::map<const DFGNode*, SyncType>& nodeSync,
        std::map<const FlopInfo*, FlopDInputDomain>& flopDSync) {
    for (const auto& flop : module.flops)
        flopDSync[&flop] = flopDInputDomainForLeaves(flopDLeaves(flop), nodeSync);

    for (const auto& sub : module.hierarchyInstantiation)
        collectFlopDSyncTypes(sub, nodeSync, flopDSync);
}

} // anonymous namespace

SyncDomainAnalysis propagateSyncDomains(
        MateIR& ir,
        const FrontendDomainFacts& domainFacts) {
    SyncDomainAnalysis analysis;
    Module& module = ir.top;
    if (!module.dfg) return analysis;

    seedDeclaredInputSyncTypes(module, {}, ir, domainFacts, analysis.node_sync);
    seedFlopSyncTypes(module, analysis.node_sync);
    analysis.node_sync = propagateNodeSyncTypes(*module.dfg, std::move(analysis.node_sync));

    assignSignalSyncTypes(module, {}, ir, domainFacts, analysis.node_sync);
    rebuildModuleNodeIndexRecursively(module);
    collectFlopDSyncTypes(module, analysis.node_sync, analysis.flop_d_domains);
    return analysis;
}

} // namespace mate
